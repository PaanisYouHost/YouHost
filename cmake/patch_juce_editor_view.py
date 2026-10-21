#!/usr/bin/env python3
"""Add a hosted plugin NSView at 1:1, in points, and fit the window to it.

JUCE puts the plugin view in its flipped, layer-backed window and sets the
frame from getAreaCoveredBy. That rectangle is multiplied by the window
scale, and the view's bounds are left alone. The drawing then scales to the
frame while clicks stay in the old bounds. Logic and Reaper add the NSView
to an unflipped parent at the view's own point size, with no AffineTransform.
"""

from pathlib import Path
import sys

NSVIEW_MARKER = "JuceHostedViewOneToOne_"
AU_MARKER = "asks the Audio Unit for its own view"
PARENT_MARKER = "The holder is already the plugin's point size."

OLD_DESTRUCTOR = """    ~NSViewAttachment() override
    {
        removeFromParent();
        [view release];
    }
"""

NEW_DESTRUCTOR = """    ~NSViewAttachment() override
    {
        removeFromParent();
        [view release];
        if (container != nil)
            [container release];
    }
"""

OLD_PLACE = """    void componentMovedOrResized (bool /*wasMoved*/, bool /*wasResized*/) override
    {
        if (auto* peer = owner.getTopLevelComponent()->getPeer())
        {
            const auto newArea = peer->getAreaCoveredBy (owner);

            if (convertToRectInt ([view frame]) != newArea)
                [view setFrame: makeCGRect (newArea)];
        }
    }
"""

NEW_PLACE = """    void componentMovedOrResized (bool /*wasMoved*/, bool /*wasResized*/) override
    {
        // Added at 1:1. The peer view is in points. getAreaCoveredBy multiplies
        // by the window scale, which would scale the drawing and leave clicks
        // in the unscaled bounds. No AffineTransform is applied to the view.
        if (container == nil)
            return;

        auto* top = owner.getTopLevelComponent();
        auto* peer = top != nullptr ? top->getPeer() : nullptr;
        if (peer == nullptr)
            return;

        auto area = peer->getAreaCoveredBy (owner);
        const float scale = top->getDesktopScaleFactor();
        if (scale > 1.01f || (scale > 0.0f && scale < 0.99f))
        {
            area.setX (roundToInt ((float) area.getX() / scale));
            area.setY (roundToInt ((float) area.getY() / scale));
            area.setWidth (roundToInt ((float) area.getWidth() / scale));
            area.setHeight (roundToInt ((float) area.getHeight() / scale));
        }

        const auto viewFrame = [view frame];
        const bool viewHasSize = viewFrame.size.width > 1.0 && viewFrame.size.height > 1.0;
        if (! adoptedViewSize && viewHasSize)
        {
            // The plugin already chose its point size. Do not squash it to the
            // placeholder component. The host window follows this view.
            area.setWidth (roundToInt ((float) viewFrame.size.width));
            area.setHeight (roundToInt ((float) viewFrame.size.height));
            adoptedViewSize = true;
        }

        if (area.getWidth() < 1 || area.getHeight() < 1)
            return;

        const NSRect holder = NSMakeRect ((CGFloat) area.getX(),
                                          (CGFloat) area.getY(),
                                          (CGFloat) area.getWidth(),
                                          (CGFloat) area.getHeight());
        if (! NSEqualRects ([container frame], holder))
            [container setFrame: holder];

        const NSRect inner = NSMakeRect (0.0, 0.0, holder.size.width, holder.size.height);
        if (! NSEqualRects ([view frame], inner))
            [view setFrame: inner];
        // Bounds match the frame, so AppKit does not scale what is drawn
        // away from the coordinates used for clicks.
        if (! NSEqualRects ([view bounds], inner))
            [view setBounds: inner];

        if (CALayer* layer = [view layer])
            if (! CGAffineTransformIsIdentity ([layer affineTransform]))
                [layer setAffineTransform: CGAffineTransformIdentity];
    }
"""

OLD_PEER = """            if (peer != nullptr)
            {
                auto peerView = (NSView*) peer->getNativeHandle();
                [peerView addSubview: view];

                componentMovedOrResized (false, false);
            }
"""

NEW_PEER = """            if (peer != nullptr)
            {
                if (container == nil)
                {
                    static struct HostViewClass final : public ObjCClass<NSView>
                    {
                        HostViewClass() : ObjCClass ("JuceHostedViewOneToOne_")
                        {
                            // Logic and Reaper give the plugin a normal unflipped
                            // parent. The JUCE window is flipped, which puts
                            // clicks in a different place from the drawing.
                            addMethod (@selector (isFlipped), [] (id, SEL) { return NO; });
                            registerClass();
                        }
                    } hostViewClass;

                    container = [hostViewClass.createInstance() init];
                    [container setAutoresizesSubviews: NO];
                    [container setWantsLayer: NO];
                }

                auto* peerView = (NSView*) peer->getNativeHandle();
                [peerView addSubview: container];
                [container addSubview: view];
                componentMovedOrResized (false, false);
            }
"""

OLD_HIDE = """        [view setHidden: ! owner.isShowing()];
"""

NEW_HIDE = """        const auto hidden = ! owner.isShowing();
        [view setHidden: hidden];
        if (container != nil)
            [container setHidden: hidden];
"""

OLD_MEMBERS = """    Component& owner;
    ComponentPeer* currentPeer = nullptr;
"""

NEW_MEMBERS = """    Component& owner;
    ComponentPeer* currentPeer = nullptr;
    NSView* container = nil;
    bool adoptedViewSize = false;
"""

OLD_REMOVE = """        if ([view superview] != nil)
            [view removeFromSuperview]; // Must be careful not to call this unless it's required - e.g. some Apple AU views
                                        // override the call and use it as a sign that they're being deleted, which breaks everything.
"""

NEW_REMOVE = """        if ([view superview] != nil)
            [view removeFromSuperview]; // Must be careful not to call this unless it's required - e.g. some Apple AU views
                                        // override the call and use it as a sign that they're being deleted, which breaks everything.
        if (container != nil && [container superview] != nil)
            [container removeFromSuperview];
"""

OLD_FIT = """        auto* view = static_cast<NSViewAttachment*> (attachment.get())->view;
        auto r = [view frame];
        setBounds (Rectangle<int> ((int) r.size.width, (int) r.size.height));

        if (auto* peer = getTopLevelComponent()->getPeer())
        {
            const auto position = peer->getAreaCoveredBy (*this).getPosition();
            [view setFrameOrigin: convertToCGPoint (position)];
        }
"""

NEW_FIT = """        auto* attached = static_cast<NSViewAttachment*> (attachment.get());
        auto r = [attached->view frame];
        setBounds (Rectangle<int> ((int) r.size.width, (int) r.size.height));
        // Position the holder. Do not move the plugin view by the peer
        // origin: that offset is outside the view's own coordinates.
        attached->componentMovedOrResized (false, false);
"""

OLD_AU = """                    pluginView = [factory uiViewForAudioUnit: plugin.getAudioUnitHandle()
                                                    withSize: NSMakeSize (getWidth(), getHeight())];
"""

NEW_AU = """                    // A zero size asks the Audio Unit for its own view, the
                    // way Logic does. 100 by 100 made Vastaus scale its drawing
                    // into a later frame and left clicks in the other space.
                    pluginView = [factory uiViewForAudioUnit: plugin.getAudioUnitHandle()
                                                    withSize: NSMakeSize (0.f, 0.f)];
"""

OLD_PARENT = """        if (auto* peer = getTopLevelComponent()->getPeer())
        {
            auto* view = static_cast<NSView*> (getView());
            const auto newArea = peer->getAreaCoveredBy (*this);
            [view setFrame: makeCGRect (newArea.withHeight (newArea.getHeight() + 1))];
            [view setFrame: makeCGRect (newArea)];
        }
"""

NEW_PARENT = """        auto* view = static_cast<NSView*> (getView());
        if (view == nil)
            return;

        // The holder is already the plugin's point size. Nudge inside it.
        // A peer rectangle here would scale or offset the view, so clicks
        // would miss the controls that are drawn.
        auto* parent = [view superview];
        if (parent == nil)
            return;

        const auto parentBounds = [parent bounds];
        [view setFrame: NSMakeRect (0.0, 0.0, parentBounds.size.width, parentBounds.size.height + 1.0)];
        [view setFrame: NSMakeRect (0.0, 0.0, parentBounds.size.width, parentBounds.size.height)];
        [view setBounds: NSMakeRect (0.0, 0.0, parentBounds.size.width, parentBounds.size.height)];
"""


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise SystemExit(f"Could not find {label}")
    return text.replace(old, new, 1)


def patch_nsview(root: Path) -> None:
    path = root / "modules/juce_gui_extra/native/juce_NSViewComponent_mac.mm"
    if not path.is_file():
        raise SystemExit(f"Could not find {path}")
    text = path.read_text(encoding="utf-8")
    if NSVIEW_MARKER in text:
        return
    text = replace_once(text, OLD_DESTRUCTOR, NEW_DESTRUCTOR, "NSView destructor")
    text = replace_once(text, OLD_PLACE, NEW_PLACE, "NSView place")
    text = replace_once(text, OLD_PEER, NEW_PEER, "NSView peer")
    text = replace_once(text, OLD_HIDE, NEW_HIDE, "NSView hide")
    text = replace_once(text, OLD_MEMBERS, NEW_MEMBERS, "NSView members")
    text = replace_once(text, OLD_REMOVE, NEW_REMOVE, "NSView remove")
    text = replace_once(text, OLD_FIT, NEW_FIT, "NSView resizeToFitView")
    path.write_text(text, encoding="utf-8")


def patch_au(root: Path) -> None:
    path = root / "modules/juce_audio_processors/format_types/juce_AudioUnitPluginFormat.mm"
    if not path.is_file():
        raise SystemExit(f"Could not find {path}")
    text = path.read_text(encoding="utf-8")
    if AU_MARKER in text:
        return
    text = replace_once(text, OLD_AU, NEW_AU, "Audio Unit withSize")
    path.write_text(text, encoding="utf-8")


def patch_parent(root: Path) -> None:
    path = root / "modules/juce_audio_processors/utilities/juce_NSViewComponentWithParent.h"
    if not path.is_file():
        raise SystemExit(f"Could not find {path}")
    text = path.read_text(encoding="utf-8")
    if PARENT_MARKER in text:
        return
    text = replace_once(text, OLD_PARENT, NEW_PARENT, "VST3 parent nudge")
    path.write_text(text, encoding="utf-8")


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    patch_nsview(root)
    patch_au(root)
    patch_parent(root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
