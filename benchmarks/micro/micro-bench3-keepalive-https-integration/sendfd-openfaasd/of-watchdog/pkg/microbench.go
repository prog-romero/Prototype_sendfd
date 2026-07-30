package pkg

import "os"

// microbenchOn gates the [MICROBENCH] measurement logs on the watchdog side
// (watchdog_recvfd_ts, and — in the CGO bridge — tls_deserialize_ns / top2).
// These write one line per migrated connection/request to stderr → journald,
// which is a significant CPU cost under load (both fwatchdog and
// systemd-journal). Disabled by default; enabled with HTTPMIGRATE_MICROBENCH=1.
// Read once at start-up. The same env var gates the C bridge (see wd_bridge.c)
// and every other component on the migration path.
var microbenchOn = os.Getenv("HTTPMIGRATE_MICROBENCH") == "1"
