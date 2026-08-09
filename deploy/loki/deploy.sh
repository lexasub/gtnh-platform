#!/usr/bin/env bash
# Deploy Loki + Promtail + Grafana on Linux (amd64, no Docker)
# Run as root on 192.168.2.109
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# -- Config --
LOKI_VER="3.4.2"
LOKI_PORT=13100
GRAFANA_VER="11.6.0"
GRAFANA_PORT=13000
BASE_URL="https://github.com/grafana/loki/releases/download/v${LOKI_VER}"
GRAFANA_URL="https://dl.grafana.com/oss/release/grafana-${GRAFANA_VER}.linux-amd64.tar.gz"
INSTALL_DIR="/opt/grafana-loki"

# -- Check root --
if [[ $EUID -ne 0 ]]; then
    echo "Run as root: sudo $0"
    exit 1
fi

# -- Install Loki + Promtail --
mkdir -p "$INSTALL_DIR"/{bin,data/loki,data/promtail,config}

cd "$INSTALL_DIR/bin"
echo "Downloading Loki ${LOKI_VER}..."
curl -fsSL "${BASE_URL}/loki-linux-amd64.zip" -o loki.zip
curl -fsSL "${BASE_URL}/promtail-linux-amd64.zip" -o promtail.zip

unzip -o loki.zip && mv loki-linux-amd64 loki && chmod +x loki
unzip -o promtail.zip && mv promtail-linux-amd64 promtail && chmod +x promtail
rm -f loki.zip promtail.zip

# -- Install Grafana --
echo "Downloading Grafana ${GRAFANA_VER}..."
curl -fsSL "$GRAFANA_URL" -o grafana.tar.gz
tar xzf grafana.tar.gz -C "$INSTALL_DIR"
rm -f grafana.tar.gz

GRAFANA_DIR="$INSTALL_DIR/grafana-v${GRAFANA_VER}"

# -- Build tcp-bridge (Go TCP→Loki) --
echo "Building tcp-bridge..."
cd "${SCRIPT_DIR}/tcp-bridge"
go build -o "$INSTALL_DIR/bin/tcp-bridge" .
cd "$OLDPWD"

# -- Loki config (v3.x) --
echo "Writing configs..."
mkdir -p /var/log/gtnh

cat > "$INSTALL_DIR/config/loki-config.yaml" <<EOF
# Loki config v3.x - non-standard port $LOKI_PORT
server:
  http_listen_port: $LOKI_PORT
  grpc_listen_port: 9096

auth_enabled: false

common:
  path_prefix: /opt/grafana-loki
  replication_factor: 1

ingester:
  wal:
    dir: $INSTALL_DIR/data/loki/wal
  lifecycler:
    ring:
      kvstore:
        store: inmemory
  chunk_idle_period: 15m
  chunk_retain_period: 5m
  max_chunk_age: 1h

schema_config:
  configs:
    - from: 2020-10-01
      store: tsdb
      object_store: filesystem
      schema: v13
      index:
        prefix: index_
        period: 24h

storage_config:
  tsdb_shipper:
    active_index_directory: $INSTALL_DIR/data/loki/tsdb-index
    cache_location: $INSTALL_DIR/data/loki/tsdb-cache
    cache_ttl: 24h
  filesystem:
    directory: $INSTALL_DIR/data/loki/chunks

compactor:
  working_directory: $INSTALL_DIR/data/loki/compactor

limits_config:
  reject_old_samples: true
  reject_old_samples_max_age: 168h
EOF

# -- Promtail config --
cat > "$INSTALL_DIR/config/promtail-config.yaml" <<EOF
# Promtail config - ships logs to loki:$LOKI_PORT
server:
  http_listen_port: 9080
  grpc_listen_port: 0

positions:
  filename: $INSTALL_DIR/data/promtail/positions.yaml

clients:
  - url: http://127.0.0.1:$LOKI_PORT/loki/api/v1/push

scrape_configs:
  - job_name: system
    static_configs:
      - targets: [localhost]
        labels:
          job: varlog
          host: gtnh-server
          __path__: /var/log/*.log

  - job_name: journal
    journal:
      max_age: 12h
      labels:
        job: systemd-journal
        host: gtnh-server
    relabel_configs:
      - source_labels: ['__journal__systemd_unit']
        target_label: 'unit'

  - job_name: gtnh-services
    static_configs:
      - targets: [localhost]
        labels:
          job: gtnh
          host: gtnh-server
          __path__: /var/log/gtnh/*.log
    pipeline_stages:
      - regex:
          expression: '\[TRACE tid=(?P<trace_id>\d+)\]\s+(?P<gtnh_svc>\S+)\s+(?P<gtnh_op>\S+)\s+(?P<gtnh_dur>\d+)us'
      - labels:
          trace_id:
          gtnh_svc:
      - metrics:
          gtnh_op_duration:
            type: Histogram
            description: "Duration per op in microseconds"
            source: gtnh_dur
            prefix: gtnh_
            buckets: [1, 10, 50, 100, 500, 1000, 5000]
            idle_duration: 1h
          gtnh_ops_total:
            type: Counter
            description: "Total operations by type"
            prefix: gtnh_
            max_idle_duration: 0
            match_all: true
            action: inc
EOF

# -- Grafana config --
GRAFANA_CONF="$GRAFANA_DIR/conf/custom.ini"
cat > "$GRAFANA_CONF" <<EOF
[server]
http_port = $GRAFANA_PORT
domain = localhost

[auth.anonymous]
enabled = true
org_role = Viewer

[users]
allow_sign_up = false
EOF

# Grafana Loki datasource provisioning
mkdir -p "$GRAFANA_DIR/conf/provisioning/datasources"
cat > "$GRAFANA_DIR/conf/provisioning/datasources/loki.yaml" <<EOF
apiVersion: 1
datasources:
  - name: Loki
    type: loki
    access: proxy
    url: http://127.0.0.1:$LOKI_PORT
    isDefault: true
    editable: false
EOF

# -- Systemd units --
echo "Writing systemd units..."
cat > /etc/systemd/system/loki.service <<EOF
[Unit]
Description=Loki log aggregation
After=network.target

[Service]
Type=simple
ExecStart=$INSTALL_DIR/bin/loki -config.file=$INSTALL_DIR/config/loki-config.yaml
Restart=always
RestartSec=5
User=root
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/promtail.service <<EOF
[Unit]
Description=Promtail log shipper
After=network.target loki.service
Requires=loki.service

[Service]
Type=simple
ExecStart=$INSTALL_DIR/bin/promtail -config.file=$INSTALL_DIR/config/promtail-config.yaml
Restart=always
RestartSec=5
User=root

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/grafana-server.service <<EOF
[Unit]
Description=Grafana dashboard
After=network.target

[Service]
Type=simple
ExecStart=$GRAFANA_DIR/bin/grafana-server -config $GRAFANA_DIR/conf/custom.ini --homepath $GRAFANA_DIR
Restart=always
RestartSec=5
User=root

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/tcp-bridge.service <<EOF
[Unit]
Description=TCP->Loki log bridge
After=network.target loki.service
Requires=loki.service

[Service]
Type=simple
ExecStart=$INSTALL_DIR/bin/tcp-bridge -listen :1514 -loki http://127.0.0.1:$LOKI_PORT/loki/api/v1/push -host gtnh-bridge
Restart=always
RestartSec=5
User=root

[Install]
WantedBy=multi-user.target
EOF

# -- Firewall --
echo "Opening ports ${LOKI_PORT}, ${GRAFANA_PORT}, and 1514..."
if command -v ufw &>/dev/null; then
    ufw allow "$LOKI_PORT/tcp" comment 'Loki'
    ufw allow "$GRAFANA_PORT/tcp" comment 'Grafana'
    ufw allow 1514/tcp comment 'tcp-bridge'
elif command -v firewall-cmd &>/dev/null; then
    firewall-cmd --add-port="${LOKI_PORT}/tcp" --permanent
    firewall-cmd --add-port="${GRAFANA_PORT}/tcp" --permanent
    firewall-cmd --reload
else
    echo "WARN: no ufw/firewall-cmd found - open ports manually"
fi

# -- Start services --
echo "Starting services..."
systemctl daemon-reload
systemctl enable loki promtail grafana-server tcp-bridge
systemctl restart loki promtail grafana-server tcp-bridge

echo ""
echo "Done! Check status:"
echo "   systemctl status loki"
echo "   systemctl status promtail"
echo "   systemctl status grafana-server"
echo ""
echo "Grafana: http://192.168.2.109:${GRAFANA_PORT}"
echo "Loki:    http://192.168.2.109:${LOKI_PORT}/ready"
echo ""
