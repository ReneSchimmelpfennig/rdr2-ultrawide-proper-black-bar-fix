# Searches the constant buffers of the final-image draws for screen geometry.
#
# The gap in the previous investigation: viewports and scissor rects were
# examined, memory was searched, but the constant buffers bound to the draws
# never were -- and that is where a UI shader normally learns how big the screen
# is. If something tells the 2D layer it is drawing into 2560x1090, it is most
# likely in here.
#
# Run from RenderDoc: Window -> Python Shell -> Open a script, then Run.

import os
import struct
import renderdoc as rd


# What a value would have to be to mean something to us, and how close counts.
TARGETS = [
    ("visible width 2560", 2560.0),
    ("visible height 1090", 1090.0),
    ("screen width 3440", 3440.0),
    ("screen height 1440", 1440.0),
    ("side bar 440", 440.0),
    ("top bar 175", 175.0),
    ("k 0.744186", 0.744186),
    ("1/k 1.343750", 1.34375),
    ("bar frac 0.127907", 0.127907),
    ("bar frac 0.121749", 0.121749),
    ("height ratio 0.756944", 0.756944),
    ("1/2560", 1.0 / 2560.0),
    ("1/1090", 1.0 / 1090.0),
    ("1/3440", 1.0 / 3440.0),
    ("1/1440", 1.0 / 1440.0),
]

RELATIVE_TOLERANCE = 1e-4


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


def describe(value):
    """Name every target this value matches, if any."""
    names = []
    for name, target in TARGETS:
        if target == 0.0:
            continue
        if abs(value - target) <= abs(target) * RELATIVE_TOLERANCE:
            names.append(name)
    return names


def scan_bytes(blob):
    """Interpret the buffer as floats and as int32 and report anything known."""
    found = []
    count = len(blob) // 4
    for i in range(count):
        word = blob[i * 4:(i + 1) * 4]
        as_float = struct.unpack("<f", word)[0]
        as_int = struct.unpack("<i", word)[0]
        for name in describe(as_float):
            found.append((i * 4, "float", as_float, name))
        if -100000 < as_int < 100000:
            for name in describe(float(as_int)):
                found.append((i * 4, "int32", as_int, name))
    return found


def buffers_for(controller, pipe, stage):
    """Every constant buffer bound to this stage, as raw bytes."""
    out = []
    try:
        refl = pipe.GetShaderReflection(stage)
    except Exception:
        return out
    if not refl:
        return out
    for index, block in enumerate(refl.constantBlocks):
        try:
            bound = pipe.GetConstantBuffer(stage, index, 0)
            if bound.resourceId == rd.ResourceId.Null():
                continue
            size = bound.byteSize
            if size == 0 or size > 65536:
                size = min(block.byteSize or 4096, 65536)
            data = controller.GetBufferData(bound.resourceId, bound.byteOffset, size)
            out.append((block.name or ("cb%d" % index), bytes(data)))
        except Exception:
            continue
    return out


def collect(controller):
    lines = []
    actions = [a for a in flatten(action_list(controller), []) if is_draw(a)]
    lines.append("draws: %d" % len(actions))

    examined = 0
    for action in actions:
        eid = event_id(action)
        controller.SetFrameEvent(eid, False)
        pipe = controller.GetPipelineState()

        try:
            outputs = pipe.GetOutputTargets()
            target = outputs[0].resource if outputs else rd.ResourceId.Null()
        except Exception:
            continue
        if target == rd.ResourceId.Null():
            continue
        if texture_size(controller, target) != (3440, 1440):
            continue  # only what writes the final image

        examined += 1
        hits = []
        for stage, label in ((rd.ShaderStage.Vertex, "vertex"),
                             (rd.ShaderStage.Pixel, "pixel")):
            for name, blob in buffers_for(controller, pipe, stage):
                for offset, kind, value, meaning in scan_bytes(blob):
                    hits.append((label, name, offset, kind, value, meaning))

        if hits:
            lines.append("")
            lines.append("event %d  (%d vertices)" % (eid, getattr(action, "numIndices", 0)))
            for label, name, offset, kind, value, meaning in hits:
                lines.append("    %-6s %-24s +0x%-4X %-5s %-14s %s" %
                             (label, name[:24], offset, kind, value, meaning))

    lines.append("")
    lines.append("examined %d draws writing the final image" % examined)
    if examined and len(lines) < 6:
        lines.append("nothing matched -- the 2D layer is not told its size this way")
    return "\n".join(lines)


def main(controller):
    text = collect(controller)
    print(text)
    path = pyrenderdoc.GetCaptureFilename()
    out = os.path.splitext(path)[0] + "_uniforms.txt" if path else "uniforms.txt"
    with open(out, "w") as handle:
        handle.write(text)
    print("\nwritten to: " + out)


pyrenderdoc.Replay().BlockInvoke(main)
