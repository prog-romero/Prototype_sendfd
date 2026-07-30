package handlers

import "os"

// microbenchOn gates the provider's [MICROBENCH] provider_sendfd_ts and the
// [MIGRATE-VERIFY] per-request trace. Both write to stderr → journald on every
// migrated request, a real CPU cost under load; disabled by default, enabled
// with HTTPMIGRATE_MICROBENCH=1 (same env var as gateway and watchdog).
var microbenchOn = os.Getenv("HTTPMIGRATE_MICROBENCH") == "1"
