# Dumps every distinct viewport, scissor rect and render-target size in a
# capture, so the 2D layer's geometry can be read off instead of guessed at.
#
# Run it from RenderDoc: Window -> Python Shell -> Open a script, then Run.
# It writes next to the capture file and prints the same thing to the console.
#
# Background: on 3440x1440 the game boxes cutscenes into 2560x1090. If the 2D
# layer is boxed by a viewport or scissor rect, those numbers show up here. If
# instead it renders into a smaller target and gets scaled, that shows up in the
# render-target sizes -- and would mean there is simply no valid content outside
# the window, which changes what a fix can achieve.

import os
import renderdoc as rd


def action_list(controller):
    """RenderDoc renamed drawcalls to actions; support both."""
    try:
        return controller.GetRootActions()
    except AttributeError:
        return controller.GetDrawcalls()


def children_of(action):
    try:
        return action.children
    except AttributeError:
        return []


def event_id(action):
    try:
        return action.eventId
    except AttributeError:
        return action.eventID


def is_draw(action):
    """Only real draws matter; markers and copies would just add noise."""
    try:
        return bool(action.flags & rd.ActionFlags.Drawcall)
    except AttributeError:
        try:
            return bool(action.flags & rd.DrawFlags.Drawcall)
        except AttributeError:
            return True


def flatten(actions, out):
    for action in actions:
        out.append(action)
        flatten(children_of(action), out)
    return out


def texture_size(controller, resource_id):
    if resource_id == rd.ResourceId.Null():
        return None
    for tex in controller.GetTextures():
        if tex.resourceId == resource_id:
            return (tex.width, tex.height)
    return None


def collect(controller):
    lines = []
    seen = {}
    order = []

    actions = flatten(action_list(controller), [])
    draws = [a for a in actions if is_draw(a)]
    lines.append("actions total: %d, of which draws: %d" % (len(actions), len(draws)))

    for action in draws:
        eid = event_id(action)
        controller.SetFrameEvent(eid, False)
        state = controller.GetPipelineState()

        try:
            vp = state.GetViewport(0)
            viewport = (round(vp.x), round(vp.y), round(vp.width), round(vp.height))
        except Exception:
            viewport = None

        try:
            sc = state.GetScissor(0)
            scissor = (sc.x, sc.y, sc.width, sc.height) if sc.enabled else None
        except Exception:
            scissor = None

        target = None
        try:
            outputs = state.GetOutputTargets()
            if outputs:
                target = texture_size(controller, outputs[0].resource)
        except Exception:
            pass

        key = (viewport, scissor, target)
        if key not in seen:
            seen[key] = [0, eid, eid]
            order.append(key)
        seen[key][0] += 1
        seen[key][2] = eid

    lines.append("")
    lines.append("%-26s %-26s %-14s %7s  %s" %
                 ("viewport x,y,w,h", "scissor x,y,w,h", "target w x h", "draws", "events"))
    lines.append("-" * 100)

    # Numbers that would mean the 2D layer is boxed into the cutscene window.
    interesting = (2560, 1090, 440, 175)

    for key in order:
        viewport, scissor, target = key
        count, first, last = seen[key]
        mark = ""
        for group in (viewport, scissor, target):
            if group and any(v in interesting for v in group):
                mark = "   <-- cutscene window geometry"
        lines.append("%-26s %-26s %-14s %7d  %d..%d%s" % (
            str(viewport), str(scissor),
            ("%dx%d" % target) if target else "-",
            count, first, last, mark))

    return "\n".join(lines)


def main(controller):
    text = collect(controller)
    print(text)

    path = pyrenderdoc.GetCaptureFilename()
    out = os.path.splitext(path)[0] + "_viewports.txt" if path else "viewports.txt"
    with open(out, "w") as handle:
        handle.write(text)
    print("\nwritten to: " + out)


pyrenderdoc.Replay().BlockInvoke(main)
