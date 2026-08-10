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

    # The numbers the game actually works in. The mesh viewer settled it: a
    # pillarbox quad arrives with x = 0.87209 and leaves as 0.74419 in clip
    # space, so the CPU hands over edges in [0,1] and the vertex shader only
    # does the usual *2-1. Searching for 2560 or for k missed this entirely --
    # the value on the wire is (1+k)/2.
    ("right edge (1+k)/2 = 0.872093", 0.872093),
    ("left edge (1-k)/2 = 0.127907", 0.127907),
    ("bottom edge 0.878251", 0.878251),
    ("top edge 0.121749", 0.121749),
    ("letterbox NDC 0.756502", 0.756502),
    ("letterbox NDC 0.243498", 0.243498),
    ("-k = -0.744186", -0.744186),
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


# Counters, so a null result can be told apart from a null attempt. The previous
# version swallowed every error and reported "nothing matched" -- which would
# have looked identical to reading no buffers at all.
STATS = {"blocks": 0, "read": 0, "bytes": 0, "errors": [], "accessor": "none"}


def unpack_binding(bound):
    """Resource, offset and size out of whatever the accessor returned.

    Newer RenderDoc hands back a UsedDescriptor wrapping a Descriptor; older
    versions returned the fields directly. Guessing wrong looks exactly like a
    negative result, so try the known shapes and report the object's actual
    fields if none of them fit.
    """
    shapes = (
        lambda b: (b.descriptor.resource, b.descriptor.byteOffset, b.descriptor.byteSize),
        lambda b: (b.descriptor.resourceId, b.descriptor.byteOffset, b.descriptor.byteSize),
        lambda b: (b.resourceId, b.byteOffset, b.byteSize),
        lambda b: (b.resource, b.byteOffset, b.byteSize),
    )
    for shape in shapes:
        try:
            return shape(bound)
        except Exception:
            continue
    if len(STATS["errors"]) < 5:
        fields = [n for n in dir(bound) if not n.startswith("_")]
        STATS["errors"].append("binding shape unknown; %s offers: %s" %
                               (type(bound).__name__, ", ".join(fields)))
    return None


def buffers_for(controller, pipe, stage):
    """Every constant buffer bound to this stage, as raw bytes."""
    out = []
    try:
        refl = pipe.GetShaderReflection(stage)
    except Exception as exc:
        STATS["errors"].append("GetShaderReflection: %s" % exc)
        return out
    if not refl:
        return out
    # The accessor has been renamed across RenderDoc versions; the first attempt
    # hard-coded GetConstantBuffer, which does not exist here, and reported a
    # null result that meant nothing. Ask the object what it actually offers.
    getter = None
    for candidate in ("GetConstantBlock", "GetConstantBuffer", "GetCBuffer"):
        if hasattr(pipe, candidate):
            getter = getattr(pipe, candidate)
            STATS["accessor"] = candidate
            break
    if getter is None:
        if not STATS["errors"]:
            available = [n for n in dir(pipe) if "onstant" in n or "Buffer" in n]
            STATS["errors"].append("no known accessor; PipeState offers: %s" % ", ".join(available))
        return out

    for index, block in enumerate(refl.constantBlocks):
        STATS["blocks"] += 1
        try:
            bound = getter(stage, index, 0)
            binding = unpack_binding(bound)
            if binding is None:
                continue
            resource, offset, size = binding
            if resource == rd.ResourceId.Null():
                continue
            if size == 0 or size > 65536:
                size = min(block.byteSize or 4096, 65536)
            data = controller.GetBufferData(resource, offset, size)
            blob = bytes(data)
            if not blob:
                continue
            STATS["read"] += 1
            STATS["bytes"] += len(blob)
            out.append((block.name or ("cb%d" % index), blob))
        except Exception as exc:
            if len(STATS["errors"]) < 5:
                STATS["errors"].append("GetConstantBuffer/GetBufferData: %s" % exc)
            continue
    return out


def collect(controller):
    lines = []
    actions = [a for a in flatten(action_list(controller), []) if is_draw(a)]
    lines.append("draws: %d" % len(actions))

    examined = 0
    samples = []
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
                samples.append((eid, "%s %s" % (label, name), blob))
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
    lines.append("constant blocks seen: %d, actually read: %d, bytes: %d, accessor: %s" %
                 (STATS["blocks"], STATS["read"], STATS["bytes"], STATS["accessor"]))
    for err in STATS["errors"]:
        lines.append("  error: %s" % err)

    if STATS["read"] == 0:
        lines.append("")
        lines.append("NOTHING WAS READ -- this says nothing about the 2D layer, only that the")
        lines.append("buffers could not be fetched. Do not read it as a negative result.")

    # Raw contents of a few draws, so the answer does not depend on my guessing
    # the right target values. Screen geometry is recognisable by eye.
    lines.append("")
    lines.append("=== raw contents of the last few final-image draws ===")
    for eid, name, blob in samples[-6:]:
        lines.append("")
        lines.append("event %d  %s  (%d bytes)" % (eid, name, len(blob)))
        words = min(len(blob) // 4, 32)
        for i in range(words):
            word = blob[i * 4:(i + 1) * 4]
            as_float = struct.unpack("<f", word)[0]
            as_int = struct.unpack("<i", word)[0]
            lines.append("    +0x%-4X  float %-16.5f int %d" % (i * 4, as_float, as_int))

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
