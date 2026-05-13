"""Decode the BA SET_OTHERMODE_H + B9 SET_OTHERMODE_L payloads in the
attribution DL to determine whether the F6 FILLRECT-clears at the end of the
DL are alpha-only (preserving RGB) — hypothesis (B) from the breakthrough
analysis.

Payloads observed in the attribution DL:
  0x80037690: BA 00170100 00000000   (sub-DL setup)
  0x80037698: BA 00080100 00000000
  0x800376A0: B9 00000200 00000000
  0x800376A8: BA 00040200 00000030
  0x800376B0: BA 00110200 00000000
  0x800376B8: BA 00090300 00000C00
  0x80720068: B9 00031D 0F0A4000   (main rendering DL — applied after op 0x02)
  0x80720070: BA 00140200 00300000

The BA opcode (G_SETOTHERMODE_H) sets a bit-range in the H half of the
combined otherMode register. Encoding (F3D / Factor 5 variant):
  w0 = 0xBA_pad_shift_length  (24-bit encoded; shift is "from bit 32 of H",
                                length is "bit count")
  w1 = data (left-shifted by shift)
The full otherMode register is 64 bits: bits 32..63 = H, 0..31 = L.
"""

# (shift, length, data) tuples for each BA / B9 command in the attribution DL.
# The encoding here follows the F3DEX/F3DFACTOR5 dispatch we already decoded:
# byte 2 of w0 = shift, byte 3 of w0 = length, w1 = data.
payloads = [
    ("0x80037690 BA", 0xBA, 0x00170100, 0x00000000),
    ("0x80037698 BA", 0xBA, 0x00080100, 0x00000000),
    ("0x800376A0 B9", 0xB9, 0x00000200, 0x00000000),
    ("0x800376A8 BA", 0xBA, 0x00040200, 0x00000030),
    ("0x800376B0 BA", 0xBA, 0x00110200, 0x00000000),
    ("0x800376B8 BA", 0xBA, 0x00090300, 0x00000C00),
    ("0x80720068 B9", 0xB9, 0x00031D00, 0x0F0A4000),
    ("0x80720070 BA", 0xBA, 0x00140200, 0x00300000),
]

# otherMode bit-field names (RDP register definitions).
# H half = bits 32..63
# L half = bits 0..31
OM_H_FIELDS = {
    # bit_offset: (name, width)
    14: ("G_MDSFT_ALPHADITHER", 2),
    16: ("G_MDSFT_RGBDITHER",   2),
    18: ("G_MDSFT_COMBKEY",     1),
    19: ("G_MDSFT_TEXTCONV",    3),
    22: ("G_MDSFT_TEXTFILT",    2),
    24: ("G_MDSFT_TEXTLUT",     2),
    26: ("G_MDSFT_TEXTLOD",     1),
    27: ("G_MDSFT_TEXTDETAIL",  2),
    29: ("G_MDSFT_TEXTPERSP",   1),
    30: ("G_MDSFT_CYCLETYPE",   2),
}
OM_L_FIELDS = {
    0:  ("G_MDSFT_ALPHACOMPARE", 2),
    2:  ("G_MDSFT_ZSRCSEL",      1),
    3:  ("G_MDSFT_RENDERMODE",  29),  # actually upper 16 bits of L
}
CYCLE_TYPES = ["1CYCLE", "2CYCLE", "COPY", "FILL"]
TEXTLUTS = ["NONE", "(reserved)", "RGBA16", "IA16"]

# Reproduce the otherMode register by applying each BA/B9 write in order.
H_reg = 0
L_reg = 0

for label, op, w0_low, w1 in payloads:
    shift = (w0_low >> 16) & 0xFF
    length = (w0_low >> 8) & 0xFF
    pad = w0_low & 0xFF
    # The "shift" field is the bit position from the high end of the register;
    # the data is written into bits [shift..shift+length-1] of the half.
    # F3D encoding: data field is placed at LSB of w1, the shift count tells
    # where in the 32-bit half it should land via a left-shift.
    # But the more common encoding is: shift = bits from low of 32-bit half;
    # we'll try both and pick the plausible decode.
    mask_low = ((1 << length) - 1) << shift
    new_data_low = (w1 << shift) & mask_low

    if op == 0xBA:
        # SETOTHERMODE_H
        H_reg = (H_reg & ~mask_low) | new_data_low
    elif op == 0xB9:
        # SETOTHERMODE_L
        L_reg = (L_reg & ~mask_low) | new_data_low

    print(f"{label}: shift={shift} len={length} pad={pad:#04x} data={w1:#010x}")
    print(f"   -> mask={mask_low:#010x} new_data={new_data_low:#010x}")
    if op == 0xBA:
        print(f"   H_reg after = {H_reg:#010x}")
    else:
        print(f"   L_reg after = {L_reg:#010x}")

print("\n=== Final otherMode state ===")
print(f"H_reg = 0x{H_reg:08X}")
print(f"L_reg = 0x{L_reg:08X}")

# Decode the H fields. Note: in the canonical RDP layout, bits are numbered
# from LSB. The "shift" in F3D's SETOTHERMODE_H actually means the bit
# position in the 32-bit H half.
print("\nDecoded H fields:")
ct = (H_reg >> 30) & 0x3
print(f"  CYCLETYPE     = {ct} ({CYCLE_TYPES[ct]})")
print(f"  TEXTPERSP     = {(H_reg >> 29) & 0x1}")
print(f"  TEXTDETAIL    = {(H_reg >> 27) & 0x3}")
print(f"  TEXTLOD       = {(H_reg >> 26) & 0x1}")
tl = (H_reg >> 24) & 0x3
print(f"  TEXTLUT       = {tl} ({TEXTLUTS[tl] if tl < len(TEXTLUTS) else 'unk'})")
print(f"  TEXTFILT      = {(H_reg >> 22) & 0x3}")
print(f"  TEXTCONV      = {(H_reg >> 19) & 0x7}")
print(f"  COMBKEY       = {(H_reg >> 18) & 0x1}")
print(f"  RGBDITHER     = {(H_reg >> 16) & 0x3}")
print(f"  ALPHADITHER   = {(H_reg >> 14) & 0x3}")

print("\nDecoded L fields:")
# L has render mode in the upper bits (3..31) and small fields in low bits
print(f"  ALPHACOMPARE  = {L_reg & 0x3}")
print(f"  ZSRCSEL       = {(L_reg >> 2) & 0x1}")
rm = L_reg & 0xFFFFFFF8  # render-mode bits
print(f"  RENDERMODE bits = 0x{rm:08X}")
print(f"    IM_RD     = {bool(rm & 0x4000)}     (read framebuffer)")
print(f"    Z_CMP     = {bool(rm & 0x0010)}     (z compare)")
print(f"    Z_UPD     = {bool(rm & 0x0020)}     (z update)")
print(f"    IM_BLEND  = {bool(rm & 0x4000)}     (alpha blend)")
print(f"    CVG_DST   = {(rm >> 8) & 0x3}       (coverage dst)")
print(f"    Z_MODE    = {(rm >> 10) & 0x3}      (z mode)")
print(f"    CVG_X_ALPHA = {bool(rm & 0x1000)}")
print(f"    ALPHA_CVG_SEL = {bool(rm & 0x2000)}")
print(f"    FORCE_BL = {bool(rm & 0x4000)}")
print(f"    Blender   [P-A-M-B] cyc1 = ({(rm >> 30) & 0x3}, {(rm >> 26) & 0x3}, {(rm >> 22) & 0x3}, {(rm >> 18) & 0x3})")
print(f"    Blender   [P-A-M-B] cyc2 = ({(rm >> 28) & 0x3}, {(rm >> 24) & 0x3}, {(rm >> 20) & 0x3}, {(rm >> 16) & 0x3})")

# Interpretation for the FILLRECT at end of DL
print("\n=== Interpretation for FILLRECT at the end of attribution DL ===")
if ct == 3:  # FILL
    print("Cycle type = FILL: F6 FILLRECT writes SET_FILL_COLOR directly to fb")
    print("In FILL cycle, the blender is bypassed entirely. Pixels are overwritten")
    print("verbatim with the fill_color value. So FILLRECT WILL wipe op 0x02's")
    print("triangle output (hypothesis B is FALSE in FILL mode).")
elif ct == 2:
    print("Cycle type = COPY: F6 FILLRECT copies texel data (uncommon for clear).")
elif ct == 0 or ct == 1:
    print(f"Cycle type = {CYCLE_TYPES[ct]}: F6 FILLRECT uses blender pipeline.")
    print("Possible that fillRect output mixes with existing fb content via")
    print("alpha-blend (hypothesis B could be TRUE — RGB preserved).")
