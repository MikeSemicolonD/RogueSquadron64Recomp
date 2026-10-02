# Long Tow Cable: the four tow-cable functions of the mission overlay, reassembled from the game with the 60-segment ring
# budget raised to SEGMENTS. Every other instruction is the original (including the co-op relocated lui at 0x800ADA48/0x800ADB00).
# Changed from the game: pool size 0x800AE13C, pool loops 0x800AE1B0/0x800AE24C, head wrap 0x800AD7E8-0x800AD820, count cap 0x800AD824, ring wraps 0x800AD8EC/0x800ADA5C/0x800ADB70/0x800ADBA4/0x800ADDD0.

# Each segment is 0.33 units; 160 holds about 4 loops around an AT-AT (the game's 60 holds about 1.5, the trip needs 3 wraps). Max 255: head and count are bytes.
    .set SEGMENTS, 160

    .set noreorder
    .set noat
    .section .recomp_patch, "ax"

# updateTowCable (decomp: updateCinematicCameraStateMachine): lays segments behind the craft, sags and releases them.
    .globl func_800AD418
    .type func_800AD418, @function
func_800AD418:
    addiu       $sp, $sp, -0xF0
    sdc1        $f20, 0xD8($sp)
    mtc1        $a2, $f20
    sw          $s6, 0xC8($sp)
    lui         $s6, 0x8011
    sw          $s3, 0xBC($sp)
    lbu         $s3, -0x4918($s6)
    sw          $s4, 0xC0($sp)
    addu        $s4, $a0, $zero
    sw          $s7, 0xCC($sp)
    addu        $s7, $a1, $zero
    sw          $s5, 0xC4($sp)
    sw          $fp, 0xD0($sp)
    addiu       $fp, $zero, 0x2
    sw          $ra, 0xD4($sp)
    sw          $s2, 0xB8($sp)
    sw          $s1, 0xB4($sp)
    sw          $s0, 0xB0($sp)
    sdc1        $f24, 0xE8($sp)
    sdc1        $f22, 0xE0($sp)
    beq         $s3, $fp, .L_800AD61C
    addiu       $s5, $s6, -0x4918
    slti        $v0, $s3, 0x3
    beq         $v0, $zero, .L_800AD48C
    addiu       $v0, $zero, 0x1
    beq         $s3, $v0, .L_800AD4A8
    nop
    j           .L_800ADD20
    nop
.L_800AD48C:
    addiu       $v0, $zero, 0x3
    beq         $s3, $v0, .L_800AD8B8
    addiu       $v0, $zero, 0x4
    beq         $s3, $v0, .L_800ADA30
    nop
    j           .L_800ADD20
    nop
.L_800AD4A8:
    lbu         $v0, 0x3($s5)
    bnel        $v0, $zero, .L_800AD4B4
    sb          $zero, 0x3($s5)
.L_800AD4B4:
    lwc1        $f0, 0x4($s5)
    lwc1        $f2, 0x0($s4)
    sub.s       $f0, $f0, $f2
    swc1        $f0, 0x10($sp)
    lwc1        $f0, 0x8($s5)
    lwc1        $f2, 0x4($s4)
    sub.s       $f0, $f0, $f2
    swc1        $f0, 0x14($sp)
    lwc1        $f0, 0xC($s5)
    lwc1        $f2, 0x8($s4)
    sub.s       $f0, $f0, $f2
    addiu       $a0, $sp, 0x10
    jal         func_8001CF2C
    swc1        $f0, 0x18($sp)
    mtc1        $zero, $f20
    mov.s       $f22, $f0
    c.eq.s      $f22, $f20
    nop
    bc1t        .L_800ADD20
    addiu       $a0, $sp, 0x90
    lui         $a1, 0x8011
    lui         $s1, 0x8011
    lwc1        $f4, 0x10($sp)
    lwc1        $f0, 0x14($sp)
    lwc1        $f2, 0x18($sp)
    lw          $s2, -0x4920($s1)
    div.s       $f4, $f4, $f22
    div.s       $f0, $f0, $f22
    div.s       $f2, $f2, $f22
    swc1        $f4, 0x10($sp)
    swc1        $f0, 0x14($sp)
    swc1        $f2, 0x18($sp)
    lwc1        $f0, 0x0($s4)
    addiu       $a1, $a1, -0x6990
    swc1        $f0, 0x54($sp)
    lwc1        $f0, 0x4($s4)
    addiu       $s0, $sp, 0x10
    swc1        $f0, 0x58($sp)
    lwc1        $f6, 0x8($s4)
    lwc1        $f0, 0x4($s0)
    lwc1        $f2, 0x8($s0)
    addu        $a2, $s0, $zero
    swc1        $f4, 0x38($sp)
    swc1        $f0, 0x44($sp)
    swc1        $f2, 0x50($sp)
    jal         func_80019548
    swc1        $f6, 0x5C($sp)
    jal         normalize_vector
    addiu       $a0, $sp, 0x90
    addiu       $a0, $sp, 0xA0
    addu        $a1, $s0, $zero
    lwc1        $f0, 0x90($sp)
    lwc1        $f2, 0x94($sp)
    lwc1        $f4, 0x98($sp)
    addiu       $a2, $sp, 0x90
    swc1        $f0, 0x30($sp)
    swc1        $f2, 0x3C($sp)
    jal         func_80019548
    swc1        $f4, 0x48($sp)
    addiu       $a0, $sp, 0x60
    lui         $a1, 0x3A2A
    ori         $a1, $a1, 0x64C3
    mfc1        $a3, $f22
    lwc1        $f0, 0xA0($sp)
    lwc1        $f2, 0xA4($sp)
    lwc1        $f4, 0xA8($sp)
    addu        $a2, $a1, $zero
    swc1        $f0, 0x34($sp)
    swc1        $f2, 0x40($sp)
    jal         func_8001CB64
    swc1        $f4, 0x4C($sp)
    addiu       $a0, $sp, 0x30
    addiu       $a1, $sp, 0x60
    jal         func_800191C4
    addiu       $a2, $s2, 0x10
    lw          $v0, -0x4920($s1)
    addu        $v1, $v0, $zero
    lw          $t0, 0x0($s4)
    lw          $t1, 0x4($s4)
    lw          $t2, 0x8($s4)
    sw          $t0, 0x0($v0)
    sw          $t1, 0x4($v0)
    sw          $t2, 0x8($v0)
    sb          $zero, 0xF5($v1)
    lw          $v0, -0x4920($s1)
    swc1        $f20, 0xC($v1)
    sb          $s3, 0xF4($v0)
    sb          $s3, 0x2($s5)
    j           .L_800ADD20
    sb          $fp, -0x4918($s6)
.L_800AD61C:
    lbu         $v0, 0x3($s5)
    bnel        $v0, $zero, .L_800AD628
    sb          $zero, 0x3($s5)
.L_800AD628:
    lui         $s2, 0x8011
    lbu         $v1, 0x1($s5)
    lw          $a0, -0x4920($s2)
    lwc1        $f2, 0x0($s4)
    sll         $v0, $v1, 5
    subu        $v0, $v0, $v1
    sll         $v0, $v0, 3
    addu        $v0, $v0, $a0
    lwc1        $f0, 0x0($v0)
    sll         $v0, $v1, 5
    subu        $v0, $v0, $v1
    sub.s       $f0, $f0, $f2
    sll         $v0, $v0, 3
    addu        $v0, $v0, $a0
    swc1        $f0, 0x10($sp)
    lwc1        $f0, 0x4($v0)
    lbu         $v1, 0x1($s5)
    lwc1        $f2, 0x4($s4)
    sll         $v0, $v1, 5
    subu        $v0, $v0, $v1
    sub.s       $f0, $f0, $f2
    sll         $v0, $v0, 3
    addu        $v0, $v0, $a0
    swc1        $f0, 0x14($sp)
    lwc1        $f0, 0x8($v0)
    lwc1        $f2, 0x8($s4)
    sub.s       $f0, $f0, $f2
    addiu       $a0, $sp, 0x10
    jal         func_8001CF2C
    swc1        $f0, 0x18($sp)
    mtc1        $zero, $f24
    mov.s       $f22, $f0
    c.eq.s      $f22, $f24
    nop
    bc1t        .L_800AD6FC
    nop
    lbu         $v1, 0x1($s5)
    sll         $v0, $v1, 5
    subu        $v0, $v0, $v1
    lw          $v1, -0x4920($s2)
    sll         $v0, $v0, 3
    addu        $v0, $v0, $v1
    addiu       $v1, $zero, 0x1
    sb          $v1, 0xF4($v0)
    lwc1        $f0, 0x10($sp)
    lwc1        $f2, 0x18($sp)
    div.s       $f0, $f0, $f22
    swc1        $f0, 0x10($sp)
    lwc1        $f0, 0x14($sp)
    div.s       $f2, $f2, $f22
    swc1        $f2, 0x18($sp)
    div.s       $f0, $f0, $f22
    swc1        $f0, 0x14($sp)
.L_800AD6FC:
    lbu         $v1, 0x1($s5)
    sll         $v0, $v1, 5
    subu        $v0, $v0, $v1
    lw          $v1, -0x4920($s2)
    sll         $v0, $v0, 3
    addu        $s1, $v0, $v1
    lbu         $v0, 0xF4($s1)
    beq         $v0, $zero, .L_800AD8B8
    addiu       $a0, $sp, 0x90
    lui         $a1, 0x8011
    lwc1        $f0, 0x0($s4)
    addiu       $a1, $a1, -0x6990
    swc1        $f0, 0x54($sp)
    lwc1        $f0, 0x4($s4)
    addiu       $s0, $sp, 0x10
    swc1        $f0, 0x58($sp)
    lwc1        $f6, 0x8($s4)
    lwc1        $f0, 0x10($sp)
    lwc1        $f2, 0x4($s0)
    lwc1        $f4, 0x8($s0)
    addu        $a2, $s0, $zero
    swc1        $f0, 0x38($sp)
    swc1        $f2, 0x44($sp)
    swc1        $f4, 0x50($sp)
    jal         func_80019548
    swc1        $f6, 0x5C($sp)
    jal         normalize_vector
    addiu       $a0, $sp, 0x90
    addiu       $a0, $sp, 0xA0
    addu        $a1, $s0, $zero
    lwc1        $f0, 0x90($sp)
    lwc1        $f2, 0x94($sp)
    lwc1        $f4, 0x98($sp)
    addiu       $a2, $sp, 0x90
    swc1        $f0, 0x30($sp)
    swc1        $f2, 0x3C($sp)
    jal         func_80019548
    swc1        $f4, 0x48($sp)
    addiu       $a0, $sp, 0x60
    lui         $a1, 0x3A2A
    ori         $a1, $a1, 0x64C3
    mfc1        $a3, $f22
    lwc1        $f0, 0xA0($sp)
    lwc1        $f2, 0xA4($sp)
    lwc1        $f4, 0xA8($sp)
    addu        $a2, $a1, $zero
    swc1        $f0, 0x34($sp)
    swc1        $f2, 0x40($sp)
    jal         func_8001CB64
    swc1        $f4, 0x4C($sp)
    addiu       $a0, $sp, 0x30
    addiu       $a1, $sp, 0x60
    jal         func_800191C4
    addiu       $a2, $s1, 0x10
    lui         $at, 0x800A
    lwc1        $f0, 0x5400($at)
    c.le.s      $f0, $f22
    nop
    bc1f        .L_800AD8B8
    nop
    lbu         $a0, 0x1($s5)
    nop
    addiu       $a0, $a0, 0x1
    sltiu       $v0, $a0, SEGMENTS
    subu        $v0, $zero, $v0
    nop
    nop
    nop
    nop
    nop
    nop
    nop
    lbu         $v1, 0x2($s5)
    and         $a0, $a0, $v0
    sltiu       $v0, $v1, SEGMENTS
    beq         $v0, $zero, .L_800AD838
    sb          $a0, 0x1($s5)
    addiu       $v0, $v1, 0x1
    sb          $v0, 0x2($s5)
.L_800AD838:
    lbu         $v1, 0x1($s5)
    sll         $v0, $v1, 5
    subu        $v0, $v0, $v1
    lw          $v1, -0x4920($s2)
    sll         $v0, $v0, 3
    addu        $v0, $v0, $v1
    lw          $t1, 0x0($s4)
    lw          $t2, 0x4($s4)
    lw          $t3, 0x8($s4)
    sw          $t1, 0x0($v0)
    sw          $t2, 0x4($v0)
    sw          $t3, 0x8($v0)
    lbu         $v1, 0x1($s5)
    lw          $a0, -0x4920($s2)
    sll         $v0, $v1, 5
    subu        $v0, $v0, $v1
    sll         $v0, $v0, 3
    addu        $v0, $v0, $a0
    swc1        $f24, 0xC($v0)
    lbu         $v1, 0x1($s5)
    sll         $v0, $v1, 5
    subu        $v0, $v0, $v1
    sll         $v0, $v0, 3
    addu        $v0, $v0, $a0
    sb          $zero, 0xF5($v0)
    lbu         $v1, 0x1($s5)
    sll         $v0, $v1, 5
    subu        $v0, $v0, $v1
    lw          $v1, -0x4920($s2)
    sll         $v0, $v0, 3
    addu        $v0, $v0, $v1
    sb          $zero, 0xF4($v0)
.L_800AD8B8:
    beq         $s7, $zero, .L_800ADD20
    addiu       $s3, $zero, 0x2
    lbu         $v0, 0x2($s5)
    lui         $at, 0x800A
    lwc1        $f0, 0x5404($at)
    slt         $v0, $s3, $v0
    mul.s       $f24, $f20, $f0
    beql        $v0, $zero, .L_800ADA20
    addiu       $s3, $zero, 0x1
    lui         $s6, 0x8011
.L_800AD8E0:
    lbu         $v0, 0x1($s5)
    subu        $s1, $v0, $s3
    bltzl       $s1, .L_800AD8F0
    addiu       $s1, $s1, SEGMENTS
.L_800AD8F0:
    sll         $v0, $s1, 5
    subu        $v0, $v0, $s1
    lw          $v1, -0x4920($s6)
    sll         $a0, $v0, 3
    addu        $v1, $a0, $v1
    lbu         $v0, 0xF4($v1)
    beq         $v0, $zero, .L_800ADA08
    nop
    lui         $at, 0x800A
    lwc1        $f20, 0x5408($at)
    addu        $s2, $zero, $zero
    addu        $s4, $a0, $zero
    addu        $s0, $s7, $zero
.L_800AD924:
    lw          $v0, -0x4920($s6)
    lwc1        $f0, 0x0($s0)
    addu        $v0, $s4, $v0
    lwc1        $f2, 0x0($v0)
    sub.s       $f0, $f0, $f2
    swc1        $f0, 0x10($sp)
    lwc1        $f0, 0x4($s0)
    lwc1        $f2, 0x4($v0)
    sub.s       $f0, $f0, $f2
    swc1        $f0, 0x14($sp)
    lwc1        $f2, 0x8($s0)
    lwc1        $f0, 0x8($v0)
    sub.s       $f2, $f2, $f0
    addiu       $a0, $sp, 0x10
    jal         func_8001CF2C
    swc1        $f2, 0x18($sp)
    mov.s       $f22, $f0
    c.lt.s      $f22, $f20
    nop
    bc1f        .L_800AD994
    addiu       $s2, $s2, 0x1
    mov.s       $f20, $f22
    lw          $t1, 0x10($sp)
    lw          $t2, 0x14($sp)
    lw          $t3, 0x18($sp)
    sw          $t1, 0x20($sp)
    sw          $t2, 0x24($sp)
    sw          $t3, 0x28($sp)
.L_800AD994:
    slti        $v0, $s2, 0x4
    bne         $v0, $zero, .L_800AD924
    addiu       $s0, $s0, 0x30
    lui         $at, 0x800A
    lwc1        $f0, 0x540C($at)
    c.lt.s      $f0, $f20
    nop
    bc1f        .L_800ADA08
    sll         $v0, $s1, 5
    div.s       $f4, $f24, $f20
    subu        $v0, $v0, $s1
    lwc1        $f2, 0x20($sp)
    lw          $v1, -0x4920($s6)
    mul.s       $f2, $f2, $f4
    sll         $v0, $v0, 3
    addu        $v0, $v0, $v1
    lwc1        $f0, 0x0($v0)
    add.s       $f0, $f0, $f2
    swc1        $f0, 0x0($v0)
    lwc1        $f2, 0x24($sp)
    mul.s       $f2, $f2, $f4
    lwc1        $f0, 0x4($v0)
    add.s       $f0, $f0, $f2
    swc1        $f0, 0x4($v0)
    lwc1        $f2, 0x28($sp)
    mul.s       $f2, $f2, $f4
    lwc1        $f0, 0x8($v0)
    add.s       $f0, $f0, $f2
    swc1        $f0, 0x8($v0)
.L_800ADA08:
    lbu         $v0, 0x2($s5)
    addiu       $s3, $s3, 0x1
    slt         $v0, $s3, $v0
    bne         $v0, $zero, .L_800AD8E0
    nop
    addiu       $s3, $zero, 0x1
.L_800ADA20:
    jal         func_800AD380
    addu        $a0, $s5, $zero
    j           .L_800ADB4C
    addu        $s7, $zero, $zero
.L_800ADA30:
    lbu         $v0, 0x2($s5)
    beq         $v0, $zero, .L_800ADAEC
    addu        $s3, $zero, $zero
    lui         $s2, 0x8011
    lui         $at, 0x800A
    lwc1        $f22, 0x5410($at)
    lui         $s6, 0x80B4
    addiu       $s4, $zero, 0x1
.L_800ADA50:
    lbu         $v0, 0x1($s5)
    subu        $s1, $v0, $s3
    bltzl       $s1, .L_800ADA60
    addiu       $s1, $s1, SEGMENTS
.L_800ADA60:
    sll         $v0, $s1, 5
    subu        $v0, $v0, $s1
    lw          $v1, -0x4920($s2)
    sll         $s1, $v0, 3
    addu        $s0, $s1, $v1
    lbu         $v0, 0xF4($s0)
    beq         $v0, $zero, .L_800ADAD8
    addiu       $a2, $s6, -0x7FA8
    mul.s       $f0, $f20, $f22
    lwc1        $f4, 0xC($s0)
    mul.s       $f4, $f4, $f20
    lwc1        $f2, 0xC($s0)
    lwc1        $f12, 0x0($s0)
    add.s       $f2, $f2, $f0
    lwc1        $f0, 0x4($s0)
    lwc1        $f14, 0x8($s0)
    add.s       $f0, $f0, $f4
    addu        $a3, $zero, $zero
    swc1        $f2, 0xC($s0)
    jal         func_80067D90
    swc1        $f0, 0x4($s0)
    lwc1        $f2, 0x4($s0)
    c.lt.s      $f0, $f2
    nop
    bc1f        .L_800ADAD8
    nop
    lw          $v0, -0x4920($s2)
    addu        $v0, $s1, $v0
    swc1        $f0, 0x4($v0)
    sb          $s4, 0xF5($v0)
.L_800ADAD8:
    lbu         $v0, 0x2($s5)
    addiu       $s3, $s3, 0x1
    slt         $v0, $s3, $v0
    bne         $v0, $zero, .L_800ADA50
    nop
.L_800ADAEC:
    lwc1        $f0, 0x4C($s5)
    mul.s       $f0, $f0, $f20
    lui         $at, 0x800A
    lwc1        $f4, 0x5414($at)
    mul.s       $f4, $f20, $f4
    lui         $a2, 0x80B4
    addiu       $a2, $a2, -0x7FA8
    lwc1        $f2, 0x44($s5)
    lwc1        $f12, 0x40($s5)
    add.s       $f2, $f2, $f0
    lwc1        $f0, 0x4C($s5)
    lwc1        $f14, 0x48($s5)
    add.s       $f0, $f0, $f4
    addu        $a3, $zero, $zero
    swc1        $f2, 0x44($s5)
    jal         func_80067D90
    swc1        $f0, 0x4C($s5)
    lwc1        $f2, 0x44($s5)
    c.lt.s      $f0, $f2
    nop
    bc1tl       .L_800ADB44
    swc1        $f0, 0x44($s5)
.L_800ADB44:
    addu        $s3, $zero, $zero
    addiu       $s7, $zero, 0x1
.L_800ADB4C:
    lbu         $v0, 0x2($s5)
    slt         $v0, $s3, $v0
    beq         $v0, $zero, .L_800ADD10
    addu        $s4, $zero, $zero
    lui         $s6, 0x8011
    addiu       $s2, $sp, 0x10
.L_800ADB64:
    lbu         $v1, 0x1($s5)
    subu        $s1, $v1, $s3
    bltzl       $s1, .L_800ADB74
    addiu       $s1, $s1, SEGMENTS
.L_800ADB74:
    sll         $v0, $s1, 5
    subu        $v0, $v0, $s1
    lw          $a1, -0x4920($s6)
    sll         $v0, $v0, 3
    addu        $a0, $v0, $a1
    lbu         $v0, 0xF4($a0)
    beq         $v0, $zero, .L_800ADCFC
    nop
    beq         $s3, $zero, .L_800ADBE4
    addiu       $v0, $v1, 0x1
    subu        $v1, $v0, $s3
    bltzl       $v1, .L_800ADBA8
    addiu       $v1, $v1, SEGMENTS
.L_800ADBA8:
    sll         $v0, $v1, 5
    subu        $v0, $v0, $v1
    sll         $v0, $v0, 3
    addu        $v0, $v0, $a1
    lwc1        $f0, 0x0($v0)
    lwc1        $f2, 0x0($a0)
    sub.s       $f0, $f0, $f2
    swc1        $f0, 0x10($sp)
    lwc1        $f0, 0x4($v0)
    lwc1        $f2, 0x4($a0)
    sub.s       $f0, $f0, $f2
    swc1        $f0, 0x14($sp)
    lwc1        $f0, 0x8($v0)
    j           .L_800ADC08
    nop
.L_800ADBE4:
    lwc1        $f0, 0x40($s5)
    lwc1        $f2, 0x0($a0)
    sub.s       $f0, $f0, $f2
    swc1        $f0, 0x10($sp)
    lwc1        $f0, 0x44($s5)
    lwc1        $f2, 0x4($a0)
    sub.s       $f0, $f0, $f2
    swc1        $f0, 0x14($sp)
    lwc1        $f0, 0x48($s5)
.L_800ADC08:
    lwc1        $f2, 0x8($a0)
    sub.s       $f0, $f0, $f2
    swc1        $f0, 0x18($sp)
    sll         $s0, $s1, 5
    subu        $s0, $s0, $s1
    lw          $v0, -0x4920($s6)
    sll         $s0, $s0, 3
    addu        $v0, $s0, $v0
    lbu         $v0, 0xF5($v0)
    addiu       $a0, $sp, 0x10
    sltiu       $v0, $v0, 0x1
    jal         normalize_vector
    or          $s4, $s4, $v0
    mov.s       $f22, $f0
    lw          $v0, -0x4920($s6)
    addiu       $a0, $sp, 0x90
    addu        $s0, $s0, $v0
    lwc1        $f0, 0x0($s0)
    lui         $a1, 0x8011
    swc1        $f0, 0x54($sp)
    lwc1        $f0, 0x4($s0)
    addiu       $a1, $a1, -0x6990
    swc1        $f0, 0x58($sp)
    lwc1        $f6, 0x8($s0)
    lwc1        $f0, 0x10($sp)
    lwc1        $f2, 0x4($s2)
    lwc1        $f4, 0x8($s2)
    addu        $a2, $s2, $zero
    swc1        $f0, 0x38($sp)
    swc1        $f2, 0x44($sp)
    swc1        $f4, 0x50($sp)
    jal         func_80019548
    swc1        $f6, 0x5C($sp)
    jal         normalize_vector
    addiu       $a0, $sp, 0x90
    addiu       $a0, $sp, 0xA0
    addu        $a1, $s2, $zero
    lwc1        $f0, 0x90($sp)
    lwc1        $f2, 0x94($sp)
    lwc1        $f4, 0x98($sp)
    addiu       $a2, $sp, 0x90
    swc1        $f0, 0x30($sp)
    swc1        $f2, 0x3C($sp)
    jal         func_80019548
    swc1        $f4, 0x48($sp)
    addiu       $a0, $sp, 0x60
    lui         $a1, 0x3A2A
    ori         $a1, $a1, 0x64C3
    mfc1        $a3, $f22
    lwc1        $f0, 0xA0($sp)
    lwc1        $f2, 0xA4($sp)
    lwc1        $f4, 0xA8($sp)
    addu        $a2, $a1, $zero
    swc1        $f0, 0x34($sp)
    swc1        $f2, 0x40($sp)
    jal         func_8001CB64
    swc1        $f4, 0x4C($sp)
    addiu       $a0, $sp, 0x30
    addiu       $a1, $sp, 0x60
    jal         func_800191C4
    addiu       $a2, $s0, 0x10
.L_800ADCFC:
    lbu         $v0, 0x2($s5)
    addiu       $s3, $s3, 0x1
    slt         $v0, $s3, $v0
    bne         $v0, $zero, .L_800ADB64
    nop
.L_800ADD10:
    bne         $s4, $zero, .L_800ADD20
    andi        $v0, $s7, 0xFF
    bnel        $v0, $zero, .L_800ADD20
    sb          $zero, 0x0($s5)
.L_800ADD20:
    lw          $ra, 0xD4($sp)
    lw          $fp, 0xD0($sp)
    lw          $s7, 0xCC($sp)
    lw          $s6, 0xC8($sp)
    lw          $s5, 0xC4($sp)
    lw          $s4, 0xC0($sp)
    lw          $s3, 0xBC($sp)
    lw          $s2, 0xB8($sp)
    lw          $s1, 0xB4($sp)
    lw          $s0, 0xB0($sp)
    ldc1        $f24, 0xE8($sp)
    ldc1        $f22, 0xE0($sp)
    ldc1        $f20, 0xD8($sp)
    jr          $ra
    addiu       $sp, $sp, 0xF0
    .size func_800AD418, . - func_800AD418

# drawTowCable (decomp: submitCameraTrackedObjectToRender): submits the live segments newest to oldest.
    .globl func_800ADD5C
    .type func_800ADD5C, @function
func_800ADD5C:
    lui         $v0, 0x8011
    lbu         $v1, -0x4918($v0)
    addiu       $sp, $sp, -0x90
    sw          $s4, 0x70($sp)
    addu        $s4, $a0, $zero
    sw          $s2, 0x68($sp)
    sw          $ra, 0x74($sp)
    sw          $s3, 0x6C($sp)
    sw          $s1, 0x64($sp)
    sw          $s0, 0x60($sp)
    sdc1        $f24, 0x88($sp)
    sdc1        $f22, 0x80($sp)
    sdc1        $f20, 0x78($sp)
    beq         $v1, $zero, .L_800ADF00
    addiu       $s2, $v0, -0x4918
    lbu         $v0, 0x2($s2)
    beq         $v0, $zero, .L_800ADF00
    addu        $s1, $zero, $zero
    lui         $v0, 0x8014
    addiu       $s3, $v0, -0x72E8
    lui         $at, 0x800A
    lwc1        $f24, 0x5418($at)
    lui         $at, 0x800A
    lwc1        $f22, 0x541C($at)
    lui         $at, 0x800A
    lwc1        $f20, 0x5420($at)
.L_800ADDC4:
    lbu         $v0, 0x1($s2)
    subu        $a0, $v0, $s1
    bltzl       $a0, .L_800ADDD4
    addiu       $a0, $a0, SEGMENTS
.L_800ADDD4:
    lui         $v1, 0x8011
    sll         $v0, $a0, 5
    subu        $v0, $v0, $a0
    lw          $v1, -0x4920($v1)
    sll         $v0, $v0, 3
    addu        $a0, $v0, $v1
    lbu         $v0, 0xF4($a0)
    beq         $v0, $zero, .L_800ADEEC
    addu        $s0, $a0, $zero
    lw          $v0, 0x4($s3)
    lwc1        $f0, 0x0($s0)
    lwc1        $f2, 0x10($v0)
    sub.s       $f0, $f0, $f2
    swc1        $f0, 0x40($sp)
    lwc1        $f0, 0x4($s0)
    lwc1        $f2, 0x14($v0)
    sub.s       $f0, $f0, $f2
    swc1        $f0, 0x44($sp)
    lwc1        $f2, 0x8($s0)
    lwc1        $f0, 0x18($v0)
    sub.s       $f2, $f2, $f0
    addiu       $a0, $sp, 0x40
    jal         func_8001CF2C
    swc1        $f2, 0x48($sp)
    lui         $at, 0x800A
    lwc1        $f2, 0x5424($at)
    sub.s       $f0, $f0, $f2
    lui         $at, 0x800A
    lwc1        $f2, 0x5428($at)
    mul.s       $f4, $f0, $f2
    c.lt.s      $f4, $f24
    nop
    bc1f        .L_800ADE64
    nop
    j           .L_800ADE74
    mov.s       $f4, $f24
.L_800ADE64:
    c.lt.s      $f22, $f4
    nop
    bc1tl       .L_800ADE74
    mov.s       $f4, $f22
.L_800ADE74:
    lwc1        $f2, 0x28($s0)
    mul.s       $f2, $f2, $f20
    lwc1        $f0, 0x0($s0)
    add.s       $f0, $f0, $f2
    swc1        $f0, 0x50($sp)
    lwc1        $f2, 0x2C($s0)
    mul.s       $f2, $f2, $f20
    lwc1        $f0, 0x4($s0)
    add.s       $f0, $f0, $f2
    swc1        $f0, 0x54($sp)
    lwc1        $f0, 0x30($s0)
    mul.s       $f0, $f0, $f20
    addiu       $a0, $sp, 0x10
    lwc1        $f2, 0x8($s0)
    mfc1        $a1, $f4
    add.s       $f2, $f2, $f0
    lui         $a3, 0x3F80
    addu        $a2, $a1, $zero
    jal         func_8001CB64
    swc1        $f2, 0x58($sp)
    addiu       $a0, $s0, 0x10
    addiu       $a1, $sp, 0x10
    jal         func_800191C4
    addiu       $a2, $s0, 0x68
    addiu       $a1, $s0, 0x40
    lw          $a0, 0x0($s4)
    lui         $a3, 0x3EA8
    ori         $a3, $a3, 0xF5C3
    jal         func_80057C8C
    addiu       $a2, $sp, 0x50
.L_800ADEEC:
    lbu         $v0, 0x2($s2)
    addiu       $s1, $s1, 0x1
    slt         $v0, $s1, $v0
    bne         $v0, $zero, .L_800ADDC4
    nop
.L_800ADF00:
    lw          $ra, 0x74($sp)
    lw          $s4, 0x70($sp)
    lw          $s3, 0x6C($sp)
    lw          $s2, 0x68($sp)
    lw          $s1, 0x64($sp)
    lw          $s0, 0x60($sp)
    ldc1        $f24, 0x88($sp)
    ldc1        $f22, 0x80($sp)
    ldc1        $f20, 0x78($sp)
    jr          $ra
    addiu       $sp, $sp, 0x90
    .size func_800ADD5C, . - func_800ADD5C

# loadTowCablePool (decomp: loadEffectModelInstancePool): allocates the segment ring and its cable meshes.
    .globl func_800AE138
    .type func_800AE138, @function
func_800AE138:
    addiu       $sp, $sp, -0x28
    ori         $a0, $zero, (SEGMENTS * 0xF8)
    addu        $a1, $zero, $zero
    sw          $ra, 0x20($sp)
    sw          $s3, 0x1C($sp)
    sw          $s2, 0x18($sp)
    sw          $s1, 0x14($sp)
    jal         rs_malloc
    sw          $s0, 0x10($sp)
    lui         $s0, 0x8011
    lui         $a0, 0x800A
    addiu       $a0, $a0, 0x53E0
    addu        $a1, $zero, $zero
    addu        $a2, $a1, $zero
    addu        $a3, $a1, $zero
    jal         load_hmt_and_hob
    sw          $v0, -0x4920($s0)
    lui         $a0, 0x800A
    jal         getHobObjectByName
    addiu       $a0, $a0, 0x53F8
    addu        $s2, $v0, $zero
    addu        $s1, $zero, $zero
    addu        $s3, $s0, $zero
    addu        $s0, $s1, $zero
.L_800AE198:
    addu        $a0, $s2, $zero
    lw          $a1, -0x4920($s3)
    addiu       $s1, $s1, 0x1
    addu        $a1, $a1, $s0
    jal         func_80059294
    addiu       $a1, $a1, 0x40
    sltiu       $v0, $s1, SEGMENTS
    bne         $v0, $zero, .L_800AE198
    addiu       $s0, $s0, 0xF8
    lui         $v0, 0x8013
    lw          $v0, 0xB50($v0)
    andi        $v0, $v0, 0x1
    bne         $v0, $zero, .L_800AE1E8
    addu        $a1, $zero, $zero
    lui         $a2, 0x3000
    lui         $v0, 0x8011
    lw          $a0, -0x4920($v0)
    addu        $a3, $a1, $zero
    jal         func_8005955C
    addiu       $a0, $a0, 0x40
.L_800AE1E8:
    lw          $ra, 0x20($sp)
    lw          $s3, 0x1C($sp)
    lw          $s2, 0x18($sp)
    lw          $s1, 0x14($sp)
    lw          $s0, 0x10($sp)
    lui         $v0, 0x8011
    sw          $zero, -0x48C8($v0)
    jr          $ra
    addiu       $sp, $sp, 0x28
    .size func_800AE138, . - func_800AE138

# freeTowCablePool (decomp: freeEffectModelInstancePool).
    .globl func_800AE218
    .type func_800AE218, @function
func_800AE218:
    addiu       $sp, $sp, -0x20
    sw          $s1, 0x14($sp)
    addu        $s1, $zero, $zero
    sw          $s2, 0x18($sp)
    lui         $s2, 0x8011
    sw          $s0, 0x10($sp)
    addu        $s0, $s1, $zero
    sw          $ra, 0x1C($sp)
.L_800AE238:
    lw          $a0, -0x4920($s2)
    addiu       $s1, $s1, 0x1
    addu        $a0, $a0, $s0
    jal         func_8005779C
    addiu       $a0, $a0, 0x40
    sltiu       $v0, $s1, SEGMENTS
    bne         $v0, $zero, .L_800AE238
    addiu       $s0, $s0, 0xF8
    lui         $v0, 0x8011
    lw          $a0, -0x4920($v0)
    jal         rs_free
    nop
    lw          $ra, 0x1C($sp)
    lw          $s2, 0x18($sp)
    lw          $s1, 0x14($sp)
    lw          $s0, 0x10($sp)
    jr          $ra
    addiu       $sp, $sp, 0x20
    .size func_800AE218, . - func_800AE218
