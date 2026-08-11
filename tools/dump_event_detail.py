# Everything about two specific draws: the ones that sample a 16:9 texture.
#
# The render-target pass found that events 7771 and 7816 read a 1920x1080
# texture and write the full 3440x1440 screen. 1920x1080 is 16:9. If that
# texture is mapped onto the 16:9 part of an ultrawide screen, the strips left
# and right have nothing valid to sample, and a clamping sampler repeats the
# edge column -- which is exactly the horizontal smearing that shows up once the
# bars are gone.
#
# This dumps the three things that would confirm or kill that:
#
#   1. the sampler address modes (clamp, wrap, border) for those draws
#   2. every constant buffer, as floats, so the UV scale is visible
#   3. all bound textures with their sizes
#
# If the address mode is clamp and a constant carries k or 1/k, the mechanism is
# settled and the fix is a UV scale rather than anything to do with layout.
#
# Run from RenderDoc: Window -> Python Shell -> Open a script, then Run.

import os
import struct
import renderdoc as rd


EVENTS = [7771, 7816]

# Values worth naming if they turn up in a constant buffer.
KNOWN = [
    ("k = (16/9)/aspect", 0.744186),
    ("1/k", 1.34375),
    ("(1-k)/2", 0.127907),
    ("(1+k)/2", 0.872093),
    ("16/9", 1.777778),
    ("aspect 3440/1440", 2.388889),
    ("9/16", 0.5625),
    ("1/1920", 1.0 / 1920.0),
    ("1/1080", 1.0 / 1080.0),
    ("1/3440", 1.0 / 3440.0),
    ("1/1440", 1.0 / 1440.0),
    ("1920", 1920.0),
    ("1080", 1080.0),
    ("3440", 3440.0),
    ("1440", 1440.0),
]
TOL = 1e-4


def name_value(v):
    out = []
    for label, target in KNOWN:
        if target != 0.0 and abs(v - target) <= abs(target) * TOL:
            out.append(label)
    return out


STATS = {"cbuffers": 0, "bytes": 0, "samplers": 0, "textures": 0, "errors": []}


def unpack_binding(bound):
    for get in (lambda b: (b.descriptor.resource, b.descriptor.byteOffset, b.descriptor.byteSize),
                lambda b: (b.descriptor.resourceId, b.descriptor.byteOffset, b.descriptor.byteSize),
                lambda b: (b.resourceId, b.byteOffset, b.byteSize),
                lambda b: (b.resource, b.byteOffset, b.byteSize)):
        try:
            return get(bound)
        except Exception:
            continue
    return None


def unpack_resource(bound):
    for get in (lambda b: b.descriptor.resource,
                lambda b: b.descriptor.resourceId,
                lambda b: b.resource,
                lambda b: b.resourceId):
        try:
            return get(bound)
        except Exception:
            continue
    return None


def address_mode_name(value):
    try:
        return str(rd.AddressMode(value))
    except Exception:
        return str(value)


def dump_event(controller, textures, eid, lines):
    controller.SetFrameEvent(eid, False)
    pipe = controller.GetPipelineState()

    lines.append("")
    lines.append("=" * 70)
    lines.append("event %d" % eid)
    lines.append("=" * 70)

    # --- textures -----------------------------------------------------------
    lines.append("")
    lines.append("-- textures bound to the pixel shader --")
    try:
        for bound in pipe.GetReadOnlyResources(rd.ShaderStage.Pixel):
            rid = unpack_resource(bound)
            if rid is None or rid == rd.ResourceId.Null():
                continue
            tex = textures.get(rid)
            if tex is None:
                continue
            STATS["textures"] += 1
            note = ""
            if (tex.width, tex.height) == (1920, 1080):
                note = "   <-- the 16:9 one"
            lines.append("   %5d x %-5d  %s%s" % (tex.width, tex.height, str(tex.format), note))
    except Exception as exc:
        STATS["errors"].append("GetReadOnlyResources: %s" % exc)

    # --- samplers -----------------------------------------------------------
    lines.append("")
    lines.append("-- samplers (the address mode is the point) --")
    try:
        for samp in pipe.GetSamplers(rd.ShaderStage.Pixel):
            desc = None
            for get in (lambda s: s.descriptor, lambda s: s):
                try:
                    candidate = get(samp)
                    if hasattr(candidate, "addressU"):
                        desc = candidate
                        break
                except Exception:
                    continue
            if desc is None:
                continue
            STATS["samplers"] += 1
            lines.append("   U=%s  V=%s  W=%s" % (address_mode_name(desc.addressU),
                                                  address_mode_name(desc.addressV),
                                                  address_mode_name(desc.addressW)))
    except Exception as exc:
        STATS["errors"].append("GetSamplers: %s" % exc)

    # --- constant buffers ---------------------------------------------------
    lines.append("")
    lines.append("-- constant buffers, as floats --")
    getter = None
    for candidate in ("GetConstantBlock", "GetConstantBuffer", "GetCBuffer"):
        if hasattr(pipe, candidate):
            getter = getattr(pipe, candidate)
            break
    if getter is None:
        lines.append("   no constant-buffer accessor on this RenderDoc version")
        return

    try:
        refl = pipe.GetShaderReflection(rd.ShaderStage.Pixel)
    except Exception as exc:
        STATS["errors"].append("GetShaderReflection: %s" % exc)
        return
    if not refl:
        lines.append("   no pixel shader reflection")
        return

    for index, block in enumerate(refl.constantBlocks):
        try:
            binding = unpack_binding(getter(rd.ShaderStage.Pixel, index, 0))
            if binding is None:
                continue
            resource, offset, size = binding
            if resource == rd.ResourceId.Null():
                continue
            if size == 0 or size > 65536:
                size = min(block.byteSize or 4096, 65536)
            blob = bytes(controller.GetBufferData(resource, offset, size))
            if not blob:
                continue
            STATS["cbuffers"] += 1
            STATS["bytes"] += len(blob)
            lines.append("   [%s] %d bytes" % (block.name or ("cb%d" % index), len(blob)))
            for i in range(min(len(blob) // 4, 64)):
                value = struct.unpack_from("<f", blob, i * 4)[0]
                names = name_value(value)
                if names:
                    lines.append("      +0x%-4X %-16.6f  %s" % (i * 4, value, ", ".join(names)))
                elif abs(value) > 1e-8:
                    lines.append("      +0x%-4X %-16.6f" % (i * 4, value))
        except Exception as exc:
            if len(STATS["errors"]) < 8:
                STATS["errors"].append("cb %d: %s" % (index, exc))


def collect(controller):
    lines = []
    textures = {t.resourceId: t for t in controller.GetTextures()}
    lines.append("looking at events: %s" % ", ".join(str(e) for e in EVENTS))

    for eid in EVENTS:
        try:
            dump_event(controller, textures, eid, lines)
        except Exception as exc:
            lines.append("event %d FAILED: %s" % (eid, exc))

    lines.append("")
    lines.append("=== summary ===")
    lines.append("constant buffers read: %d (%d bytes), samplers: %d, textures: %d" %
                 (STATS["cbuffers"], STATS["bytes"], STATS["samplers"], STATS["textures"]))
    for err in STATS["errors"]:
        lines.append("  error: %s" % err)
    if STATS["cbuffers"] == 0 and STATS["samplers"] == 0:
        lines.append("")
        lines.append("NOTHING WAS READ. Not a negative result -- a failed one.")
    return "\n".join(lines)


def main(controller):
    text = collect(controller)
    print(text)
    path = pyrenderdoc.GetCaptureFilename()
    out = os.path.splitext(path)[0] + "_events.txt" if path else "events.txt"
    with open(out, "w") as handle:
        handle.write(text)
    print("\nwritten to: " + out)


pyrenderdoc.Replay().BlockInvoke(main)
