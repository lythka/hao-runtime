import Foundation
// A struct initializer shaped like a private framework's metrics value (tests/guest/stackpackfp.sh):
// two resilient values and an existential by address (x0-x2), four doubles of insets (d0-d3), two
// closures and an Optional object (x3-x7), then past x7 and d7 two objects, five CGFloat? (payload
// word and tag byte), two Bools and a CGFloat. Apple arm64 packs the tags and Bools at their natural
// size; SysV gives each an 8-byte slot, the trailing CGFloat included.
public struct FontSrc {
    public var name: String
    public init(_ name: String) { self.name = name }
}
public protocol Dim { var value: Double { get } }
public struct StaticDim: Dim {
    public var value: Double
    public init(_ v: Double) { value = v }
}

public struct Metrics {
    public var desc: String
    @inline(never)
    public init(fontSource: FontSrc, subtitleFontSource: FontSrc, inAppPurchaseTextSpace: any Dim,
                contentInsets: NSEdgeInsets, redownloadImage: @autoclosure () -> NSObject,
                pauseImage: @autoclosure () -> NSObject, pausedImageSymbolConfiguration: NSObject?,
                symbolConfiguration: NSObject, redownloadImageSymbolConfiguration: NSObject?,
                minimumSize: CGSize, progressDiameter: CGFloat, lineWidth: CGFloat,
                textShapeLineWidth: CGFloat?, expandsToFit: Bool, cornerRadius: CGFloat?,
                includeTopPadding: Bool, resumeImageNavigationHeight: CGFloat?,
                resumeImageNavigationBaselineOffset: CGFloat?, subtitleMaxWidth: CGFloat?,
                multilineSubtitleTopPaddingScale: CGFloat) {
        func o(_ v: CGFloat?) -> String { v.map { "\($0)" } ?? "nil" }
        desc = "fs=\(fontSource.name) sfs=\(subtitleFontSource.name) iap=\(inAppPurchaseTextSpace.value) " +
            "insets=\(contentInsets.top),\(contentInsets.left),\(contentInsets.bottom),\(contentInsets.right) " +
            "redownload=\(redownloadImage()) pause=\(pauseImage()) " +
            "paused=\(pausedImageSymbolConfiguration.map { "\($0)" } ?? "nil") conf=\(symbolConfiguration) " +
            "redl=\(redownloadImageSymbolConfiguration.map { "\($0)" } ?? "nil") " +
            "min=\(minimumSize.width)x\(minimumSize.height) prog=\(progressDiameter) line=\(lineWidth) " +
            "shape=\(o(textShapeLineWidth)) expands=\(expandsToFit) corner=\(o(cornerRadius)) " +
            "top=\(includeTopPadding) resH=\(o(resumeImageNavigationHeight)) " +
            "resB=\(o(resumeImageNavigationBaselineOffset)) subMax=\(o(subtitleMaxWidth)) " +
            "scale=\(multilineSubtitleTopPaddingScale)"
    }
}
