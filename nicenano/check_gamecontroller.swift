// Lists the controllers Apple's GameController framework sees and prints every
// input change for a few seconds. This is the same framework most native Mac
// games use.
//
//   swiftc -O check_gamecontroller.swift -o check_gamecontroller
//   ./check_gamecontroller [seconds]      (default 12)

import Foundation
import GameController

setvbuf(stdout, nil, _IOLBF, 0)   // line-buffered, so output shows up live when redirected to a file
let seconds = Double(CommandLine.arguments.dropFirst().first ?? "") ?? 12
GCController.shouldMonitorBackgroundEvents = true   // we are a terminal tool, not the frontmost app

let start = Date()
func stamp() -> String { String(format: "%7.2fs", Date().timeIntervalSince(start)) }

func describe(_ c: GCController) -> String {
    let kind = c.extendedGamepad != nil ? "extended gamepad" :
               (c.microGamepad != nil ? "micro gamepad" : "no gamepad profile")
    return "\"\(c.vendorName ?? "?")\"  category=\(c.productCategory)  profile=\(kind)"
}

var watched = Set<ObjectIdentifier>()
var lastXY = [ObjectIdentifier: (Float, Float)]()

func watch(_ c: GCController) {
    guard let pad = c.extendedGamepad, !watched.contains(ObjectIdentifier(c)) else { return }
    watched.insert(ObjectIdentifier(c))
    pad.valueChangedHandler = { _, element in
        let name = element.localizedName ?? "\(type(of: element))"
        if let button = element as? GCControllerButtonInput {
            if button.isAnalog && button.value > 0 && button.value < 1 { return }  // only ends of a trigger pull
            print("\(stamp())  \(name): \(button.isPressed ? "pressed" : "released")  value=\(String(format: "%.2f", button.value))")
        } else if let dpad = element as? GCControllerDirectionPad {
            let id = ObjectIdentifier(dpad)
            let (lx, ly) = lastXY[id] ?? (0, 0)
            let (x, y) = (dpad.xAxis.value, dpad.yAxis.value)
            if abs(x - lx) > 0.4 || abs(y - ly) > 0.4 || (x == 0 && y == 0 && (lx != 0 || ly != 0)) {
                print("\(stamp())  \(name): x=\(String(format: "%+.2f", x)) y=\(String(format: "%+.2f", y))")
                lastXY[id] = (x, y)
            }
        }
    }
}

NotificationCenter.default.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { n in
    if let c = n.object as? GCController {
        print("connected:  " + describe(c))
        watch(c)
    }
}
NotificationCenter.default.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { n in
    if let c = n.object as? GCController { print("disconnected: " + describe(c)) }
}

// Controllers that were already connected before we started
DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
    let list = GCController.controllers()
    print("controllers known to GameController: \(list.count)")
    for c in list { print("  - " + describe(c)); watch(c) }
}

DispatchQueue.main.asyncAfter(deadline: .now() + seconds) {
    print("done")
    exit(0)
}
RunLoop.main.run()
