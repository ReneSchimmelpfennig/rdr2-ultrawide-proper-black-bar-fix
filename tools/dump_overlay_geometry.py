# Which draws are confined to the 16:9 box, and which fill the screen?
#
# The 2D problem is not the subtitles -- it is the full-screen overlays in the
# intro and the credit inserts. Those cover only the 2560x1090 window, and the
# strip the bars used to hide shows artefacts because nothing was composed
# there.
#
# A quad covering the whole screen leaves the vertex shader at NDC x = +-1.
# One confined to the box leaves at +-k = +-0.744186, which is exactly what the
# mesh viewer showed for a pillarbox quad (VS input 0.87209 -> output 0.74419).
# So the clip-space bounding box of every draw tells us, without guessing, which
# geometry is boxed in.
#
# The point is to come away with the exact float the CPU put on the wire. That
# is a value we can then hunt in memory with a watchpoint -- the method that
# located the field of view.
#
# Run from RenderDoc: Window -> Python Shell -> Open a script, then Run.

import os
import struct
import renderdoc as rd


K = 0.744186          # (16/9) / (3440/1440)
TOLERANCE = 0.01      # generous: we are classifying, not measuring


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


def texture_size(controller, resource_id):
    for tex in controller.GetTextures():
        if tex.resourceId == resource_id:
            return (tex.width, tex.height)
    return None


# Counters, so "nothing found" can be told apart from "nothing examined". Two
# earlier scripts in this project reported a clean negative while reading zero
# bytes, and it cost a day.
STATS = {"draws": 0, "fullres": 0, "meshes": 0, "empty": 0, "errors": []}


def postvs_positions(controller, action):
    """Clip-space positions leaving the vertex shader, as (x, y, z, w) tuples."""
    try:
        postvs = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
    except Exception as exc:
        if len(STATS["errors"]) < 5:
            STATS["errors"].append("GetPostVSData: %s" % exc)
        return []

    if postvs.vertexResourceId == rd.ResourceId.Null():
        STATS["empty"] += 1
        return []

    count = postvs.numIndices
    if count == 0 or count > 4096:
        return []

    stride = postvs.vertexByteStride
    if stride == 0:
        return []

    try:
        raw = bytes(controller.GetBufferData(postvs.vertexResourceId,
                                             postvs.vertexByteOffset,
                                             stride * count))
    except Exception as exc:
        if len(STATS["errors"]) < 5:
            STATS["errors"].append("GetBufferData: %s" % exc)
        return []

    out = []
    for i in range(count):
        base = i * stride
        if base + 16 > len(raw):
            break
        out.append(struct.unpack_from("<4f", raw, base))
    if out:
        STATS["meshes"] += 1
    return out


def classify(min_x, max_x):
    """What the horizontal extent says about this geometry."""
    def near(value, target):
        return abs(abs(value) - target) < TOLERANCE

    if near(min_x, 1.0) and near(max_x, 1.0):
        return "FULL SCREEN"
    if near(min_x, K) and near(max_x, K):
        return "BOXED  <-- confined to the 16:9 window"
    return ""


def collect(controller):
    lines = []
    actions = [a for a in flatten(action_list(controller), []) if is_draw(a)]
    lines.append("draws in this frame: %d" % len(actions))
    lines.append("k = %.6f, so a boxed quad leaves the vertex shader at NDC x = +-%.6f" % (K, K))
    lines.append("")

    boxed = []

    for action in actions:
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
        if texture_size(controller, target) != (3440, 1440):
            continue
        STATS["fullres"] += 1

        positions = postvs_positions(controller, action)
        if not positions:
            continue

        ndc = []
        for x, y, z, w in positions:
            if abs(w) < 1e-9:
                continue
            ndc.append((x / w, y / w))
        if not ndc:
            continue

        xs = [p[0] for p in ndc]
        ys = [p[1] for p in ndc]
        min_x, max_x = min(xs), max(xs)
        min_y, max_y = min(ys), max(ys)

        verdict = classify(min_x, max_x)
        # Only small geometry is interesting here: overlays and inserts are
        # quads, not meshes with thousands of vertices.
        if len(ndc) > 64 and not verdict:
            continue

        row = ("event %-6d  verts %-5d  x [%+.5f .. %+.5f]  y [%+.5f .. %+.5f]  %s" %
               (eid, len(ndc), min_x, max_x, min_y, max_y, verdict))
        lines.append(row)
        if "BOXED" in verdict:
            boxed.append((eid, min_x, max_x, min_y, max_y))

    lines.append("")
    lines.append("=== summary ===")
    lines.append("draws examined: %d, writing the final image: %d, with readable meshes: %d" %
                 (STATS["draws"], STATS["fullres"], STATS["meshes"]))
    for err in STATS["errors"]:
        lines.append("  error: %s" % err)

    if STATS["meshes"] == 0:
        lines.append("")
        lines.append("NO MESH DATA WAS READ AT ALL. This says nothing about the geometry, only")
        lines.append("that it could not be fetched. Do not read it as a negative result.")
    elif not boxed:
        lines.append("")
        lines.append("No draw is confined to the box. If the overlays still cover only the")
        lines.append("2560-wide region, they are being clipped or sampled that way rather than")
        lines.append("built that way -- which points at the pixel shader, not the geometry.")
    else:
        lines.append("")
        lines.append("%d boxed draw(s). These are the ones to chase:" % len(boxed))
        for eid, min_x, max_x, min_y, max_y in boxed:
            # What the CPU would have written in [0,1] space before the usual
            # *2-1 in the vertex shader. That is the number to hunt in memory.
            left = (min_x + 1.0) / 2.0
            right = (max_x + 1.0) / 2.0
            lines.append("  event %d: on the wire the CPU wrote about %.6f and %.6f" %
                         (eid, left, right))

    return "\n".join(lines)


def main(controller):
    text = collect(controller)
    print(text)
    path = pyrenderdoc.GetCaptureFilename()
    out = os.path.splitext(path)[0] + "_overlay.txt" if path else "overlay.txt"
    with open(out, "w") as handle:
        handle.write(text)
    print("\nwritten to: " + out)


pyrenderdoc.Replay().BlockInvoke(main)
