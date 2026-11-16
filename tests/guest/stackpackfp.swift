// Guest calls of a native Swift initializer whose packed stack arguments end with a CGFloat
// (tests/guest/stackpackfp.sh, library tests/guest/stackpackfp_lib.swift). Each line is the
// values as the native initializer received them.
import Foundation
import SPFLib

final class Named: NSObject {
    let n: String
    init(_ n: String) { self.n = n }
    override var description: String { n }
}

// the words past the packed arguments hold stale values
@inline(never) func dirty() -> Int {
    var junk = [Int](repeating: 0x1_10cc_3702, count: 64)
    return withUnsafeMutableBytes(of: &junk) { $0.count }
}

@inline(never) func make(_ shape: CGFloat?, _ expands: Bool, _ corner: CGFloat?, _ top: Bool,
                         _ resH: CGFloat?, _ resB: CGFloat?, _ sub: CGFloat?, _ scale: CGFloat,
                         _ paused: NSObject?, _ redl: NSObject?) -> Metrics {
    Metrics(fontSource: FontSrc("body"), subtitleFontSource: FontSrc("caption"),
            inAppPurchaseTextSpace: StaticDim(12), contentInsets: NSEdgeInsets(top: 6, left: 12, bottom: 6, right: 12),
            redownloadImage: Named("cloud"), pauseImage: Named("pause"), pausedImageSymbolConfiguration: paused,
            symbolConfiguration: Named("conf"), redownloadImageSymbolConfiguration: redl,
            minimumSize: CGSize(width: 66, height: 32), progressDiameter: 30, lineWidth: 3,
            textShapeLineWidth: shape, expandsToFit: expands, cornerRadius: corner, includeTopPadding: top,
            resumeImageNavigationHeight: resH, resumeImageNavigationBaselineOffset: resB,
            subtitleMaxWidth: sub, multilineSubtitleTopPaddingScale: scale)
}

let cases: [(CGFloat?, Bool, CGFloat?, Bool, CGFloat?, CGFloat?, CGFloat?, CGFloat, NSObject?, NSObject?)] = [
    (nil, false, nil, true, nil, nil, nil, 1, nil, Named("redl")),
    (nil, false, 8.5, true, nil, nil, nil, 1, nil, Named("redl")),
    (nil, false, nil, true, nil, nil, 120, 0.75, Named("paused"), nil),
    (1.5, true, 14, false, 22, -3.25, 200, 2, Named("paused"), Named("redl")),
]
for c in cases {
    _ = dirty()
    print(make(c.0, c.1, c.2, c.3, c.4, c.5, c.6, c.7, c.8, c.9).desc)
}
