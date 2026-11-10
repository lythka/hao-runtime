// The program's entry point called by native code (tests/guest/swiftthunks.sh): swiftc records the @main
// function in __TEXT,__swift5_entry ({rel32 main, u32 flags}), and ExtensionFoundation's
// +[_EXUtil _getEntryPointFunction] (an .appex's NSExtensionMain) reads it from the main executable's header
// with getsectiondata and calls it. swiftentryhelper.c does the same; the second entry prints and returns.
@_silgen_name("swiftentry_call") func swiftentry_call() -> Int32

nonisolated(unsafe) var entered = 0

@main struct SwiftEntry {
    static func main() {
        entered += 1
        if entered > 1 {
            print("entered again through __swift5_entry (\(entered))")
            return
        }
        print("first entry, through LC_MAIN")
        print("native call of the entry point returned \(swiftentry_call())")
        print("entries: \(entered)")
    }
}
