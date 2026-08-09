# Настройка логов GTNH сервисов для Loki

Все сервисы используют spdlog. Нужно перенаправить stderr каждого сервиса
в файл `/var/log/gtnh/<service>.log`:

```bash
# В run.sh или systemd unit'е:
./chunkd 2>&1 | tee -a /var/log/gtnh/chunkd.log &
./simcored 2>&1 | tee -a /var/log/gtnh/simcored.log &
./gatewayd 2>&1 | tee -a /var/log/gtnh/gatewayd.log &
./client 2>&1 | tee -a /var/log/gtnh/client.log &
```

### Уровень логирования

TRACE логи пишутся на `spdlog::info`, так что их видно при любом уровне
выше `warn`. Если что-то не видно — поставь:

```bash
export GTNH_LOG_LEVEL=info   # или debug
```

### Проверка

После запуска всех сервисов на сервере и клиенте:

```bash
# На машине с Loki (192.168.2.109):
grep 'TRACE tid=' /var/log/gtnh/*.log

# Должно быть что-то вроде:
# [TRACE tid=7] cas_cb complete 150us
# [TRACE tid=7] simcore publish_block_changed 42us
# [TRACE tid=7] gateway relay_block_changed 200us
# [TRACE tid=7] client recv_block_changed 0us
```

### Loki/Grafana

```
Grafana: http://192.168.2.109:13000
Loki:    http://192.168.2.109:13100/ready
```

В Grafana -> Explore -> Loki -> поиск:
```
{trace_id="7"}
```
Покажет все логи с этим trace_id.

Или через метки:
```
{gtnh_svc="simcore"}
```
