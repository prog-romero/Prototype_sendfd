//go:build linux

package httpmigrate

import "os"

// microbenchOn gates the [MICROBENCH] measurement logs (serialization time,
// top1, …). These logs write one line per connection/request to stderr, which
// journald then persists — under load that alone burns a lot of CPU (both the
// process and systemd-journal). They are therefore DISABLED by default and only
// emitted when HTTPMIGRATE_MICROBENCH=1, so a throughput/CPU evaluation is not
// skewed by logging. The flag is read once at start-up (env is immutable here).
//
// The same variable name is honoured by every component on the migration path
// (gateway, faasd-provider, watchdog, both Go and C), so exporting
// HTTPMIGRATE_MICROBENCH=1 in all of them turns the measurements on together.
var microbenchOn = os.Getenv("HTTPMIGRATE_MICROBENCH") == "1"
