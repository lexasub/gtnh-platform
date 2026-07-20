package metrics

import (
	"fmt"
	"log"
	"os"
	"time"
)

func CheckVersionAndExit(serviceName string) {
	for _, arg := range os.Args[1:] {
		if arg == "--version" || arg == "-v" {
			fmt.Println(serviceName)
			fmt.Println("Version: (not configured)")
			fmt.Println("Git Hash: (not configured)")
			fmt.Println("Build Date: (not configured)")
			os.Exit(0)
		}
	}
}

func FormatUptime(startTime time.Time) string {
	uptime := time.Since(startTime)
	d := int(uptime.Hours() / 24)
	h := int(uptime.Hours()) % 24
	m := int(uptime.Minutes()) % 60
	s := int(uptime.Seconds()) % 60
	return fmt.Sprintf("%d days, %02d:%02d:%02d", d, h, m, s)
}

func PrintMetricsHeader(serviceName string) {
	log.Println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
	log.Println("METRICS:", serviceName)
	log.Println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
}

func PrintMetricsFooter() {
	log.Println("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━")
}
