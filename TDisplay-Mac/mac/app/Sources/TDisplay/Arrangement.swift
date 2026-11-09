// How the tablet's screen relates to the Mac's main screen, chosen from the menu and kept in the app's preferences.
// Extending makes the virtual display and puts it on one side of the main screen. Mirroring makes no virtual display at
// all and shows the main screen itself on the tablet, scaled to fit, rather than using macOS's own mirroring, which can
// change the main screen's resolution to match.
import CoreGraphics
import Foundation

enum Arrangement: String, CaseIterable {
    case right, left, above, below, mirror

    private static let key = "arrangement"

    static var current: Arrangement {
        get { UserDefaults.standard.string(forKey: key).flatMap(Arrangement.init) ?? .right }
        set { UserDefaults.standard.set(newValue.rawValue, forKey: key) }
    }

    var title: String {
        switch self {
        case .right: return "Extend to the right"
        case .left: return "Extend to the left"
        case .above: return "Extend above"
        case .below: return "Extend below"
        case .mirror: return "Mirror the main screen"
        }
    }

    // Where a display of this size goes, against the main screen and centred along it. Nil for mirroring.
    func origin(for size: CGSize) -> CGPoint? {
        let main = CGDisplayBounds(CGMainDisplayID())
        switch self {
        case .right: return CGPoint(x: main.maxX, y: main.midY - size.height / 2)
        case .left: return CGPoint(x: main.minX - size.width, y: main.midY - size.height / 2)
        case .above: return CGPoint(x: main.midX - size.width / 2, y: main.minY - size.height)
        case .below: return CGPoint(x: main.midX - size.width / 2, y: main.maxY)
        case .mirror: return nil
        }
    }
}

// The part of the tablet's picture that the main screen fills when mirrored, as fractions of the picture: the whole height
// or the whole width, centred, whichever keeps the main screen's shape.
func fittedArea(of screen: CGSize, in picture: CGSize) -> CGRect {
    guard screen.width > 0, screen.height > 0, picture.width > 0, picture.height > 0 else { return CGRect(x: 0, y: 0, width: 1, height: 1) }
    let screenAspect = screen.width / screen.height
    let pictureAspect = picture.width / picture.height
    if screenAspect > pictureAspect {
        let height = pictureAspect / screenAspect
        return CGRect(x: 0, y: (1 - height) / 2, width: 1, height: height)
    }
    let width = screenAspect / pictureAspect
    return CGRect(x: (1 - width) / 2, y: 0, width: width, height: 1)
}
