.syntax unified
.thumb
.fpu fpv4-sp-d16

.section .text.fx_hooks, "ax", %progbits

/* ============================================================================
 * 1. Audio post-mixer hook
 *    DOOM Suite:  patched at 0x83FE8010 (inside bnc_tx, right after blx 0x1C2D1)
 *                 chains into bnc_tx_inner (0x83FE8141), returns to 0x0001D09B.
 *    Stock Suite: patched at 0x0001D090 (runs mixer 0x0001C2D1 first, then DSP,
 *                 returns to 0x0001D09B).
 *    r5 holds the audio struct pointer; *(int32_t **)r5 is the 64x16 SAI TX buffer.
 * ============================================================================ */
.global fx_audio_hook_stock
.thumb_func
fx_audio_hook_stock:
    mov     r0, r5
    mov.w   r1, #0x1000
    movw    ip, #0xc2d1
    movt    ip, #0x0001
    blx     ip
    /* fall through into common post-mixer DSP wrapper (without bnc_tx) */
    push    {r0-r4, lr}
    vmrs    r4, fpscr
    push    {r4, r6}            /* 2 words -> maintains 8-byte SP alignment */
    vpush   {d0-d15}
    /* Enable Flush-to-Zero (bit 24) and Default NaN (bit 25) */
    orr     r4, r4, #0x03000000
    vmsr    fpscr, r4
    ldr     r0, [r5]
    movw    r1, #0xd3a0
    movt    r1, #0x202a
    bl      fx_process_block
    vpop    {d0-d15}
    pop     {r4, r6}
    vmsr    fpscr, r4
    pop     {r0-r4, lr}
    movw    ip, #0xd09b
    movt    ip, #0x0001
    bx      ip

.global fx_audio_hook_doom
.thumb_func
fx_audio_hook_doom:
    push    {r0-r4, lr}
    vmrs    r4, fpscr
    push    {r4, r6}            /* 2 words -> maintains 8-byte SP alignment */
    vpush   {d0-d15}
    /* Enable Flush-to-Zero (bit 24) and Default NaN (bit 25) */
    orr     r4, r4, #0x03000000
    vmsr    fpscr, r4
    ldr     r0, [r5]
    movw    r1, #0xd3a0
    movt    r1, #0x202a
    bl      fx_process_block
    vpop    {d0-d15}
    pop     {r4, r6}
    vmsr    fpscr, r4
    pop     {r0-r4, lr}
    /* Chain to SP-ETC Bounce TX observer (0x83FE8141), then return to 0x0001D09B */
    movw    ip, #0x8141
    movt    ip, #0x83fe
    blx     ip
    movw    ip, #0xd09b
    movt    ip, #0x0001
    bx      ip

/* ============================================================================
 * 2. DT1 builder entry hook (0x800D8118)
 *    Args: r0=link, r1=param_id, r2=index, r3=value
 *    - Swallows param IDs 627..662 (never sent to BMC).
 *    - On FxType (0x7E..0x82): calls fx_on_select(slot, value) and remaps
 *      49..54 -> 0 (Bypass) before chaining to fde_dt1 (DOOM) or stock (Stock).
 * ============================================================================ */
.global fx_dt1_hook_doom
.thumb_func
fx_dt1_hook_doom:
    /* Check param IDs 627..698 */
    movw    ip, #627
    cmp     r1, ip
    blo     1f
    movw    ip, #698
    cmp     r1, ip
    bhi     1f
    movs    r0, #0
    bx      lr
1:
    /* Check FxType 0x7E..0x82 */
    sub.w   ip, r1, #0x7e
    cmp.w   ip, #4
    bhi     2f
    push    {r0-r3, r4, lr}
    mov     r0, ip
    mov     r1, r3
    bl      fx_on_select
    str     r0, [sp, #12]       /* overwrite saved r3 with remapped FxType (0 for 49..60) */
    pop     {r0-r3, r4, lr}
2:
    /* Chain to SP-ETC fde_dt1 at 0x83FCA5CB */
    movw    ip, #0xa5cb
    movt    ip, #0x83fc
    bx      ip

.global fx_dt1_hook_stock
.thumb_func
fx_dt1_hook_stock:
    movw    ip, #627
    cmp     r1, ip
    blo     1f
    movw    ip, #698
    cmp     r1, ip
    bhi     1f
    movs    r0, #0
    bx      lr
1:
    sub.w   ip, r1, #0x7e
    cmp.w   ip, #4
    bhi     2f
    push    {r0-r3, r4, lr}
    mov     r0, ip
    mov     r1, r3
    bl      fx_on_select
    str     r0, [sp, #12]
    pop     {r0-r3, r4, lr}
2:
    /* Replay displaced 10-byte prologue of 0x800D8118 and jump to 0x800D8123 */
    push.w  {r4, r5, r6, r7, r8, sb, sl, fp, lr}
    sub     sp, #12
    movw    sb, #0xe1c8
    movw    ip, #0x8123
    movt    ip, #0x800d
    bx      ip

/* ============================================================================
 * 3. SetParam CTRL hook (0x80018A98)
 *    Args: r0=project, r1=field(0x27F..0x284), r2=value, r3=bus, [sp+0]=effect
 *    For effect >= 49: stores into g_fx.val + updates DSP shadow state,
 *    never touches SYSPRM + 0xCE..+0xD3F (fixes BUS 4 corruption!).
 * ============================================================================ */
.global fx_set_ctrl_hook
.thumb_func
fx_set_ctrl_hook:
    ldr     ip, [sp, #0]
    cmp     ip, #49
    blo     1f
    push    {r4, lr}
    mov     r0, r3              /* arg0: bus */
    mov     r3, r2              /* arg3: value */
    subw    r2, r1, #0x27f      /* arg2: knob 0..5 */
    mov     r1, ip              /* arg1: effect 49..53 */
    bl      fx_set_ctrl_ext
    pop     {r4, pc}
1:
    /* Replay displaced 10 bytes of 0x80018A98 and jump to 0x80018AA3 */
    push.w  {r4, r5, r6, r7, r8, sb, sl, lr}
    subw    r6, r1, #0x27f
    mov     r5, r1
    movw    ip, #0x8aa3
    movt    ip, #0x8001
    bx      ip

/* ============================================================================
 * 4. Dirty-flag byte write hooks (0x800DE800 and 0x800DEB40)
 *    Prevents strb [SYSPRM + bus*49 + fx + 0xC4A] OOB write when fx >= 49.
 * ============================================================================ */
.global fx_dirty_hook_1
.thumb_func
fx_dirty_hook_1:
    /* At 0x800DE800: sl=SYSPRM, r7=bus, r4=effect; sets dirty=1 */
    cmp     r4, #49
    blo     1f
    push    {r0-r3, lr}
    mov     r0, r7
    mov     r1, r4
    movs    r2, #1
    bl      fx_set_dirty_ext
    pop     {r0-r3, lr}
    movw    ip, #0xe80f
    movt    ip, #0x800d
    bx      ip
1:
    movs    r0, #0x31
    movs    r1, #1
    mla     r0, r7, r0, sl
    add     r0, r4
    movw    ip, #0xe80b
    movt    ip, #0x800d
    bx      ip

.global fx_dirty_hook_2
.thumb_func
fx_dirty_hook_2:
    /* At 0x800DEB40: sl=SYSPRM, r7=bus, [sp,#4]=effect, r8=value */
    ldr     r1, [sp, #4]
    cmp     r1, #49
    blo     1f
    push    {r0-r3, lr}
    mov     r0, r7
    movs    r2, #0
    cmp.w   r8, #0
    it      ne
    movne   r2, #1
    bl      fx_set_dirty_ext
    pop     {r0-r3, lr}
    movw    ip, #0xe80f
    movt    ip, #0x800d
    bx      ip
1:
    movs    r0, #0x31
    mla     r0, r7, r0, sl
    cmp.w   r8, #0
    add     r0, r1
    mov     r1, r8
    it      ne
    movne   r1, #1
    movw    ip, #0xe80b
    movt    ip, #0x800d
    bx      ip

/* ============================================================================
 * 5. SYS_GET hook (at 0x800DA17C, 10 bytes)
 *    Live regs: r5=SYSPRM, r7=field, sb=idx, r0=resolved_default_bus (0..4)
 *    If field in 0x27F..0x285 and active fx >= 49:
 *      r8 = fx_get_param_ext(bus, fx, field), jump to 0x800DA4DB.
 * ============================================================================ */
.global fx_get_param_hook
.thumb_func
fx_get_param_hook:
    subw    r3, r7, #0x27f
    cmp     r3, #6
    bhi     1f
    /* Resolve bus: (uint16_t)sb <= 4 ? (uint16_t)sb : r0 */
    uxth.w  r2, sb
    cmp     r2, #4
    it      hi
    movhi   r2, r0
    add.w   r1, r5, r2, lsl #1
    ldrsh.w r1, [r1, #0xc4]     /* r1 = active fx on bus */
    cmp     r1, #49
    blo     1f
    push    {r0-r3, lr}
    mov     r0, r2              /* arg0: bus */
    /* r1 already = effect */
    mov     r2, r7              /* arg2: field (0x27F..0x285) */
    bl      fx_get_param_ext
    mov     r8, r0
    pop     {r0-r3, lr}
    movw    ip, #0xa4db
    movt    ip, #0x800d
    bx      ip
1:
    /* Replay displaced 10 bytes at 0x800DA17C */
    subw    r3, r7, #0x275
    cmp     r3, #0x10
    bhi     2f
    movw    ip, #0xa187
    movt    ip, #0x800d
    bx      ip
2:
    movw    ip, #0xa4db
    movt    ip, #0x800d
    bx      ip

/* ============================================================================
 * 6. OLED value formatter hook (at 0x800D595A, 10 bytes)
 *    Live regs: sb=SYSPRM, r8=out_buf, r7=field, r0=bus (0..4)
 *    If field in 0x27F..0x284 and active fx >= 49:
 *      calls fx_format_param(out_buf, bus, fx, knob), jumps to 0x800D5995.
 * ============================================================================ */
.global fx_fmt_param_hook
.thumb_func
fx_fmt_param_hook:
    subw    r2, r7, #0x27f
    cmp     r2, #5
    bhi     1f
    add.w   r1, sb, r0, lsl #1
    ldrsh.w r1, [r1, #0xc4]     /* r1 = active fx on bus */
    cmp     r1, #49
    blo     1f
    push    {r0-r3, lr}
    mov     r3, r2              /* arg3: knob 0..5 */
    mov     r2, r1              /* arg2: effect */
    mov     r1, r0              /* arg1: bus */
    mov     r0, r8              /* arg0: out_buf */
    bl      fx_format_param
    pop     {r0-r3, lr}
    movw    ip, #0x5995
    movt    ip, #0x800d
    bx      ip
1:
    /* Replay displaced 10 bytes at 0x800D595A */
    subw    r2, r7, #0x273
    cmp     r2, #0x11
    bhi     2f
    movw    ip, #0x5965
    movt    ip, #0x800d
    bx      ip
2:
    movw    ip, #0x5ab1
    movt    ip, #0x800d
    bx      ip

/* ============================================================================
 * 7. MFX grid pad-LED T3' lookup helper (hooked at 0x8002649A)
 *    Inputs: r1 = T3' base (49 u32s), r4 = current fx
 *    Output: r6 = grid index (0..48, or 0 if not found), returns to 0x8002A57D.
 * ============================================================================ */
.global fx_find_t3_led
.thumb_func
fx_find_t3_led:
    movs    r6, #0
1:
    ldr.w   r2, [r1, r6, lsl #2]
    cmp     r2, r4
    beq     2f
    adds    r6, #1
    cmp     r6, #54
    blo     1b
    movs    r6, #0
2:
    movw    ip, #0xa57d
    movt    ip, #0x8002
    bx      ip

/* ============================================================================
 * 8. 4th MFX Grid Page header title hook (hooked at 0x8014615A, 12 bytes)
 *    Inputs: r0 = mfx_page (1..4), r4 = FX View object
 *    Draws " MFX LIST 49-64" when page == 4, otherwise dispatches stock 1..3.
 * ============================================================================ */
.global fx_page4_title_hook
.thumb_func
fx_page4_title_hook:
    cmp     r0, #4
    beq     1f
    cmp     r0, #3
    beq     2f
    cmp     r0, #2
    beq     3f
    cmp     r0, #1
    beq     4f
    movw    ip, #0x6135
    movt    ip, #0x8014
    bx      ip
1:
    ldr.w   r0, [r4, #0x84]
    movs    r1, #0x40
    movs    r2, #5
    adr     r3, k_mfx_list_49_64
    movw    ip, #0x4fa1
    movt    ip, #0x800d
    blx     ip
    movw    ip, #0x6135
    movt    ip, #0x8014
    bx      ip
2:
    movw    ip, #0x6179
    movt    ip, #0x8014
    bx      ip
3:
    movw    ip, #0x6189
    movt    ip, #0x8014
    bx      ip
4:
    movw    ip, #0x6167
    movt    ip, #0x8014
    bx      ip

.align 2
k_mfx_list_49_64:
    .asciz " MFX LIST 49-64"
.align 2

/* ============================================================================
 * 9. 4th MFX Grid Page button shortcut hook (hooked at 0x80146920, 16 bytes)
 *    While holding MFX:
 *      - DJFX (0x2F) + ISOLATOR (0x2E) together -> Page 4 (4/4)
 *      - DJFX (0x2F) alone                      -> Page 2 (2/4)
 *      - ISOLATOR (0x2E) alone                  -> Page 3 (3/4)
 *      - Neither                                -> Page 1 (1/4)
 *    Returns to 0x80146AF7 with r0 = page (r4 is dead and reloaded at 0x80146AF8).
 * ============================================================================ */
.global fx_page4_keys_hook
.thumb_func
fx_page4_keys_hook:
    movs    r0, #0x2f
    movw    ip, #0x9f21
    movt    ip, #0x800d
    blx     ip
    mov     r4, r0              /* r4 = key(0x2F) */
    movs    r0, #0x2e
    movw    ip, #0x9f21
    movt    ip, #0x800d
    blx     ip                  /* r0 = key(0x2E) */
    cmp     r4, #0
    beq     1f
    cmp     r0, #0
    ite     ne
    movne   r0, #4              /* both 0x2F + 0x2E -> Page 4 */
    moveq   r0, #2              /* 0x2F only        -> Page 2 */
    b       2f
1:
    cmp     r0, #0
    ite     ne
    movne   r0, #3              /* 0x2E only        -> Page 3 */
    moveq   r0, #1              /* neither          -> Page 1 */
2:
    movw    ip, #0x6af7
    movt    ip, #0x8014
    bx      ip
