# Details for every draw that writes the final full-resolution image.
#
# The viewport dump narrowed things down to a handful of draws on the 3440x1440
# target, one of them scissored to a vertically cropped strip. This says what
# those draws actually are: name, shaders, output texture and geometry, so the
# pass responsible for the artefacts can be named instead of guessed at.
#
# Run from RenderDoc: Window -> Python Shell -> Open a script, then Run.

import os
import renderdoc as rd


def action_list(controller):
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


def action_name(controller, action):
    for attempt in (
        lambda: action.GetName(controller.GetStructuredFile()),
        lambda: action.name,
        lambda: action.customName,
    ):
        try:
            name = attempt()
            if name:
                return str(name)
        except Exception:
            pass
    return "<unnamed>"


def is_draw(action):
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


def resource_name(controller, resource_id):
    if resource_id == rd.ResourceId.Null():
        return "-"
    for res in controller.GetResources():
        if res.resourceId == resource_id:
            return res.name
    return str(resource_id)


def texture_size(controller, resource_id):
    for tex in controller.GetTextures():
        if tex.resourceId == resource_id:
            return (tex.width, tex.height)
    return None


def shader_of(controller, state, stage):
    try:
        sid = state.GetShader(stage)
        if sid == rd.ResourceId.Null():
            return None
        try:
            entry = state.GetShaderEntryPoint(stage)
        except Exception:
            entry = ""
        return "%s %s" % (resource_name(controller, sid), entry)
    except Exception:
        return None


def collect(controller):
    lines = []
    actions = [a for a in flatten(action_list(controller), []) if is_draw(a)]

    for action in actions:
        eid = event_id(action)
        controller.SetFrameEvent(eid, False)
        state = controller.GetPipelineState()

        try:
            outputs = state.GetOutputTargets()
            target_id = outputs[0].resource if outputs else rd.ResourceId.Null()
        except Exception:
            continue
        if target_id == rd.ResourceId.Null():
            continue

        size = texture_size(controller, target_id)
        if size != (3440, 1440):
            continue  # only the final image matters here

        try:
            sc = state.GetScissor(0)
            scissor = (sc.x, sc.y, sc.width, sc.height) if sc.enabled else None
        except Exception:
            scissor = None

        cropped = scissor is not None and scissor[3] < 1440

        lines.append("")
        lines.append("event %d   %s%s" % (eid, action_name(controller, action),
                                          "     <<< SCISSOR CROPS THE IMAGE" if cropped else ""))
        lines.append("    target   %s  %dx%d" % (resource_name(controller, target_id),
                                                 size[0], size[1]))
        lines.append("    scissor  %s" % str(scissor))
        try:
            lines.append("    geometry vertices=%d instances=%d" %
                         (action.numIndices, action.numInstances))
        except Exception:
            pass
        for stage, label in ((rd.ShaderStage.Vertex, "vertex"),
                             (rd.ShaderStage.Pixel, "pixel")):
            shader = shader_of(controller, state, stage)
            if shader:
                lines.append("    %-8s %s" % (label, shader))

    if not lines:
        lines.append("no draws found writing a 3440x1440 target")
    return "\n".join(lines)


def main(controller):
    text = collect(controller)
    print(text)
    path = pyrenderdoc.GetCaptureFilename()
    out = os.path.splitext(path)[0] + "_fullres.txt" if path else "fullres.txt"
    with open(out, "w") as handle:
        handle.write(text)
    print("\nwritten to: " + out)


pyrenderdoc.Replay().BlockInvoke(main)
