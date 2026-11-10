// Swift's logging routes from guest code (tests/guest/oslog.sh, src/oslog.c): Logger (the compiler's
// buffer, _os_log_impl with the module's #dsohandle), os_log() and os_signpost() (libswiftos, native code
// that sends a pack naming the guest's dso: os_log_pack_send, _os_signpost_emit_with_name_impl).
import Foundation
import os

let tag = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "tag"
let logger = Logger(subsystem: "com.example.hao.oslog", category: "swift")
let log = OSLog(subsystem: "com.example.hao.oslog", category: "swiftlegacy")
print(getpid())
fflush(stdout)
let secret = "SWIFT-SECRET"
let value = 17
logger.log("\(tag, privacy: .public) swift start")
logger.info("logger public \(secret, privacy: .public) private \(secret, privacy: .private) default \(secret) int \(value)")
logger.error("logger error \(value * 2) \(3.25, format: .fixed(precision: 2))")
logger.debug("logger debug \(value, privacy: .private(mask: .hash))")
os_log("os_log %{public}@ %d %{private}@", log: log, type: .default, "pub" as NSString, value, "priv" as NSString)
os_log("os_log error %{public}s", log: log, type: .error, "err")
let spid = OSSignpostID(log: log)
os_signpost(.begin, log: log, name: "SwiftInterval", signpostID: spid, "begin %d", value)
os_signpost(.end, log: log, name: "SwiftInterval", signpostID: spid)
logger.log("\(tag, privacy: .public) swift end")
