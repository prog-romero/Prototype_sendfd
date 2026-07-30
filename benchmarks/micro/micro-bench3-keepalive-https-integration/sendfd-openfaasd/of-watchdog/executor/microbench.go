package executor

import "os"

// microbenchOn gates the [MICROBENCH] vanilla_top2_ns log emitted once the
// watchdog has read the whole request. One line per request → journald, which
// is costly under load; disabled by default, enabled with
// HTTPMIGRATE_MICROBENCH=1 (same env var as the rest of the migration path).
var microbenchOn = os.Getenv("HTTPMIGRATE_MICROBENCH") == "1"
