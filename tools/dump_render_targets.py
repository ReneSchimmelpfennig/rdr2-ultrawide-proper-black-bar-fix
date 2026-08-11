# Is there an intermediate buffer that is only valid inside the 16:9 box?
#
# The geometry scan settled that the overlays are drawn full screen: nothing in
# the frame is built at +-k except the bars themselves. So the artefacts outside
# the 2560-wide region are not a shape problem -- something the overlays *read*
# is only correct in the middle.
#
# That matches the pixel history from earlier: a broken pixel and a good pixel
# take the same passes in the same order, and the difference is already in what
# feeds them.
#
# Two things are looked for here, both of which the previous passes never
# examined:
#
#   1. Every render target in the frame and its size. A buffer at 2560x1090,
#      2560x1440, or any half/quarter of those, is the smoking gun. Constant
#      buffers were searched for those numbers and came back clean -- but the
#      textures themselves never were.
#   2. For the full-screen draws near the end of the frame, which textures the
#      pixel shader reads, and how big they are. If an overlay samples something
#      smaller than the screen, its edges have nowhere valid to sample from.
#
# Run from RenderDoc: Window -> Python Shell -> Open a script, then Run.

import os
import renderdoc as rd


SCREEN = (3440, 1440)

# Sizes worth shouting about: the framed window, the 16:9 box, and the usual
# downsampled versions of both.
SUSPECT = {}
for w, h in ((2560, 1090), (2560, 1440), (3440, 1440)):
    for div in (1, 2, 4, 8):
        SUSPECT[(w // div, h // div)] = "%dx%d / %d" % (w, h, div)


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


STATS = {"textures": 0, "targets": 0, "draws": 0, "inputs_read": 0, "errors": []}


def unpack_resource(bound):
    """The resource id out of whatever shape this RenderDoc version returns."""
    for get in (lambda b: b.descriptor.resource,
                lambda b: b.descriptor.resourceId,
                lambda b: b.resource,
                lambda b: b.resourceId):
        try:
            return get(bound)
        except Exception:
            continue
    return None


def collect(controller):
    lines = []
    textures = {t.resourceId: t for t in controller.GetTextures()}
    STATS["textures"] = len(textures)

    # --- 1. every render target, by size -------------------------------------
    sizes = {}
    for tex in textures.values():
        try:
            is_target = bool(tex.creationFlags & rd.TextureCategory.ColorTarget)
        except Exception:
            is_target = True
        if not is_target:
            continue
        STATS["targets"] += 1
        key = (tex.width, tex.height)
        sizes[key] = sizes.get(key, 0) + 1

    lines.append("=== render targets in this frame, by size ===")
    for key in sorted(sizes, key=lambda s: (-s[0] * s[1], s)):
        w, h = key
        note = ""
        if key in SUSPECT:
            note = "   <-- %s" % SUSPECT[key]
        elif h and abs(w / float(h) - 2560.0 / 1090.0) < 0.01:
            note = "   <-- same shape as the framed window"
        lines.append("  %5d x %-5d   %3d target(s)%s" % (w, h, sizes[key], note))

    hits = [k for k in sizes if k in SUSPECT and k != SCREEN]
    lines.append("")
    if hits:
        lines.append("A buffer the size of the framed window exists. That is the thing to chase.")
    else:
        lines.append("No render target matches the framed window at any scale. So nothing is")
        lines.append("rendered into a smaller box -- the overlays read a full-size buffer whose")
        lines.append("*contents* are wrong at the edges, which moves the question upstream again.")

    # --- 2. what the last full-screen draws read -----------------------------
    actions = [a for a in flatten(action_list(controller), []) if is_draw(a)]
    lines.append("")
    lines.append("=== inputs of the last full-screen draws (the overlays and the bars) ===")

    tail = actions[-40:]
    for action in tail:
        eid = event_id(action)
        controller.SetFrameEvent(eid, False)
        pipe = controller.GetPipelineState()
        STATS["draws"] += 1

        try:
            outputs = pipe.GetOutputTargets()
            target = outputs[0].resource if outputs else rd.ResourceId.Null()
        except Exception:
            continue
        if target == rd.ResourceId.Null():
            continue
        out_tex = textures.get(target)
        if out_tex is None or (out_tex.width, out_tex.height) != SCREEN:
            continue

        reads = []
        try:
            for bound in pipe.GetReadOnlyResources(rd.ShaderStage.Pixel):
                rid = unpack_resource(bound)
                if rid is None or rid == rd.ResourceId.Null():
                    continue
                tex = textures.get(rid)
                if tex is None:
                    continue
                STATS["inputs_read"] += 1
                mark = ""
                if (tex.width, tex.height) != SCREEN:
                    mark = "  <-- not the screen size"
                reads.append("%dx%d%s" % (tex.width, tex.height, mark))
        except Exception as exc:
            if len(STATS["errors"]) < 5:
                STATS["errors"].append("GetReadOnlyResources: %s" % exc)

        if reads:
            lines.append("  event %-6d reads: %s" % (eid, ", ".join(sorted(set(reads)))))
        else:
            lines.append("  event %-6d reads: (none reported)" % eid)

    lines.append("")
    lines.append("=== summary ===")
    lines.append("textures: %d, of them colour targets: %d; draws examined: %d, inputs read: %d" %
                 (STATS["textures"], STATS["targets"], STATS["draws"], STATS["inputs_read"]))
    for err in STATS["errors"]:
        lines.append("  error: %s" % err)
    if STATS["targets"] == 0:
        lines.append("")
        lines.append("NO TARGETS WERE CLASSIFIED. This says nothing about the buffers, only that")
        lines.append("they could not be read. Do not take it as a negative result.")

    return "\n".join(lines)


def main(controller):
    text = collect(controller)
    print(text)
    path = pyrenderdoc.GetCaptureFilename()
    out = os.path.splitext(path)[0] + "_targets.txt" if path else "targets.txt"
    with open(out, "w") as handle:
        handle.write(text)
    print("\nwritten to: " + out)


pyrenderdoc.Replay().BlockInvoke(main)
