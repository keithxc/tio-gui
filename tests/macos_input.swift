// SPDX-License-Identifier: GPL-3.0-only
// Test input is restricted to the explicitly supplied PID and its foreground window.
import AppKit
import ApplicationServices
import Carbon
let args=CommandLine.arguments
let pid=pid_t(args[1])!, app=AXUIElementCreateApplication(pid)
var result:CFTypeRef?
guard AXUIElementCopyAttributeValue(app,kAXWindowsAttribute as CFString,&result) == .success,
 let windows=result as? [AXUIElement],let window=windows.first else { exit(2) }
if NSWorkspace.shared.frontmostApplication?.processIdentifier != pid {
 _ = AXUIElementPerformAction(window,kAXRaiseAction as CFString)
 NSRunningApplication(processIdentifier:pid)?.activate(options:[])
 Thread.sleep(forTimeInterval:0.4)
}
// Input sources are per-app on macOS. Physical ASCII keycodes otherwise enter
// Chinese IME composition. Restore the user's source after this scoped action.
let previousSource=TISCopyCurrentKeyboardInputSource().takeRetainedValue()
let asciiSources=["com.apple.keylayout.US","com.apple.keylayout.ABC"].flatMap { id -> [TISInputSource] in
 guard let list=TISCreateInputSourceList([kTISPropertyInputSourceID as String:id] as CFDictionary,false) else {return []}
 return list.takeRetainedValue() as! [TISInputSource]
}
if args[2] == "key" || args[2] == "text" {
 guard let ascii=asciiSources.first,TISSelectInputSource(ascii) == noErr else {exit(8)}
 Thread.sleep(forTimeInterval:0.2)
}
defer {
 if args[2] == "key" || args[2] == "text" {Thread.sleep(forTimeInterval:0.3);TISSelectInputSource(previousSource)}
}
func postKey(_ code:CGKeyCode) {
 let down=CGEvent(keyboardEventSource:nil,virtualKey:code,keyDown:true)!,up=CGEvent(keyboardEventSource:nil,virtualKey:code,keyDown:false)!
 down.flags=[];up.flags=[]
 guard NSWorkspace.shared.frontmostApplication?.processIdentifier == pid else {exit(4)}
 down.post(tap:.cghidEventTap);Thread.sleep(forTimeInterval:0.1);up.post(tap:.cghidEventTap);Thread.sleep(forTimeInterval:0.1)
}
if args[2] == "key" { postKey(CGKeyCode(args[3])!) }
if args[2] == "text" {
 let keys:[Character:CGKeyCode] = ["b":11,"u":32,"n":45,"d":2,"l":37,"e":14,"-":27,"s":1,"m":46,"o":31,"k":40]
 for character in args[3] {
  guard let code=keys[character] else {exit(6)}
  postKey(code)
 }
}
if args[2] == "click" {
 var p:CFTypeRef?,s:CFTypeRef?
 guard AXUIElementCopyAttributeValue(window,kAXPositionAttribute as CFString,&p) == .success,
 AXUIElementCopyAttributeValue(window,kAXSizeAttribute as CFString,&s) == .success else {exit(3)}
 var point=CGPoint.zero,size=CGSize.zero
 AXValueGetValue(p as! AXValue,.cgPoint,&point);AXValueGetValue(s as! AXValue,.cgSize,&size)
 let x=point.x+Double(args[3])!*size.width, y=point.y+size.height-Double(args[4])!
 let moved=CGEvent(mouseEventSource:nil,mouseType:.mouseMoved,mouseCursorPosition:CGPoint(x:x,y:y),mouseButton:.left)!
 let down=CGEvent(mouseEventSource:nil,mouseType:.leftMouseDown,mouseCursorPosition:CGPoint(x:x,y:y),mouseButton:.left)!
 let up=CGEvent(mouseEventSource:nil,mouseType:.leftMouseUp,mouseCursorPosition:CGPoint(x:x,y:y),mouseButton:.left)!
 moved.flags=[];down.flags=[];up.flags=[]
 down.setIntegerValueField(.mouseEventClickState,value:1);up.setIntegerValueField(.mouseEventClickState,value:1)
 guard NSWorkspace.shared.frontmostApplication?.processIdentifier == pid else {exit(4)}
 moved.post(tap:.cghidEventTap);Thread.sleep(forTimeInterval:0.2)
 down.post(tap:.cghidEventTap);Thread.sleep(forTimeInterval:0.05);up.post(tap:.cghidEventTap)
 print("Clicked app window",pid,Int(size.width),Int(size.height))
}

if args[2] == "snapshot" {
 let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly,.excludeDesktopElements],kCGNullWindowID) as? [[String:Any]] ?? []
 if let record=list.first(where:{($0[kCGWindowOwnerPID as String] as? Int)==Int(pid) && ($0[kCGWindowLayer as String] as? Int)==0}),let id=record[kCGWindowNumber as String] as? Int {
  let capture=Process();capture.executableURL=URL(fileURLWithPath:"/usr/sbin/screencapture");capture.arguments=["-x","-l",String(id),args[3]];try capture.run();capture.waitUntilExit();exit(capture.terminationStatus)
 }
 exit(5)
}
