; Standalone STE test of the Timer-B beam-timed palette raster used by the
; scummvm-ste-scene backend (backends/graphics/atari/atari-ste-raster.S). It
; installs the same two-part mechanism -- a VBL vector hook that presents a
; frame and arms MFP Timer B, and a Timer-B handler that synchronises to the
; first display line through the video counter and then runs the proven
; per-line Spectrum palette loop (ste/raster.s) with interrupts masked -- and
; plays a schedule in which every scanline carries a distinct solid colour.
; The screen is left all-zero, so each display row shows palette register 0,
; i.e. that line's colour; timerb-raster-test.mjs checks every row.
;
; Schedule layout (words), identical to atari-ste-raster.h:
;   0..15    palette of lines 0 and 1 (written at VBL)
;   16..31   line 1's border group: the palette line 2 starts with
;   then 48 words per line y = 2..LINES-1: group1 (left), group2 (mid),
;   border (right, = line y+1's start).
LINES       equ 200

raster_frame: macro
        lea     delay\@,a6
.wait\@:
        cmp.b   (a5),d1
        beq.s   .wait\@
        move.b  (a5),d0
        sub.b   d1,d0
        add.l   d0,a6
        jmp     (a6)
delay\@:
        rept 75
        nop
        endr
        lea     (a4),a2
        rept 8
        move.l  (a3)+,(a2)+
        endr
        rept \1
        nop
        endr
.line\@:
        nop
        lea     (a4),a0
        lea     (a4),a1
        lea     (a4),a2
        rept 8
        move.l  (a3)+,(a0)+
        endr
        rept 8
        move.l  (a3)+,(a1)+
        endr
        rept 8
        move.l  (a3)+,(a2)+
        endr
        ifne \2
        nop
        endc
        dbf     d7,.line\@
        bra     tb_done
        endm


        text
start:
        move.l  sp,a5
        lea     stack_top,sp
        move.l  4(a5),a5
        move.l  $c(a5),d0
        add.l   $14(a5),d0
        add.l   $1c(a5),d0
        addi.l  #$100,d0
        move.l  d0,-(sp)
        move.l  a5,-(sp)
        clr.w   -(sp)
        move.w  #$4a,-(sp)      ; Mshrink
        trap    #1
        lea     12(sp),sp

        pea     check_ste
        move.w  #$26,-(sp)      ; Supexec
        trap    #14
        addq.l  #6,sp
        tst.w   d0
        beq     exit_program

        move.w  #-1,-(sp)
        move.w  #-1,-(sp)
        move.w  #0,-(sp)        ; Setscreen: keep, inquire resolution
        move.w  #4,-(sp)
        trap    #14
        addq.l  #2,sp
        addq.l  #6,sp
        move.w  d0,old_resolution
        cmpi.w  #2,d0
        beq     exit_program

        move.w  #2,-(sp)        ; Physbase
        trap    #14
        addq.l  #2,sp
        move.l  d0,old_physbase
        move.w  #3,-(sp)        ; Logbase
        trap    #14
        addq.l  #2,sp
        move.l  d0,old_logbase

        ; A page-aligned zeroed screen.
        lea     screen_storage,a0
        move.l  a0,d0
        addi.l  #255,d0
        andi.l  #$ffffff00,d0
        move.l  d0,screen_base
        move.l  d0,a1
        move.w  #(32000/4)-1,d7
.clear:
        clr.l   (a1)+
        dbf     d7,.clear

        pea     mouse_off
        clr.w   -(sp)
        move.w  #$19,-(sp)      ; Ssbrk trick? no: line-A mouse off via trap-14 22
        trap    #14
        addq.l  #8,sp

        move.w  #0,-(sp)        ; low resolution
        move.l  screen_base,-(sp)
        move.l  screen_base,-(sp)
        move.w  #5,-(sp)        ; Setscreen
        trap    #14
        lea     12(sp),sp

        pea     install
        move.w  #$26,-(sp)
        trap    #14
        addq.l  #6,sp

        ; Present the schedule + screen.
        move.l  #schedule,pending_palette
        move.l  screen_base,pending_screen

.wait_present:
        tst.l   pending_screen
        bne.s   .wait_present

main_loop:
        move.w  #$25,-(sp)      ; Vsync
        trap    #14
        addq.l  #2,sp
        move.w  #2,-(sp)        ; Cconis
        move.w  #1,-(sp)
        trap    #13
        addq.l  #4,sp
        tst.l   d0
        beq.s   main_loop
        move.w  #2,-(sp)
        move.w  #2,-(sp)        ; Cnecin
        trap    #13
        addq.l  #4,sp

cleanup:
        pea     uninstall
        move.w  #$26,-(sp)
        trap    #14
        addq.l  #6,sp
        move.w  old_resolution,-(sp)
        move.l  old_physbase,-(sp)
        move.l  old_logbase,-(sp)
        move.w  #5,-(sp)
        trap    #14
        lea     12(sp),sp
        pea     mouse_on
        clr.w   -(sp)
        move.w  #$19,-(sp)
        trap    #14
        addq.l  #8,sp
exit_program:
        clr.w   -(sp)
        trap    #1

check_ste:
        move.l  $5a0.w,d0
        beq.s   .no
        move.l  d0,a0
.cookie:
        move.l  (a0)+,d0
        beq.s   .no
        move.l  (a0)+,d1
        cmpi.l  #'_MCH',d0
        bne.s   .cookie
        swap    d1
        cmpi.w  #1,d1
        blt.s   .no             ; STE (1) or better
        moveq   #1,d0
        rts
.no:
        moveq   #0,d0
        rts

; ---------------------------------------------------------------------------
install:
        move.w  sr,-(sp)
        move.w  #$2700,sr
        move.b  $ffff820a.w,old_sync
        clr.b   $ffff820f.w
        clr.b   $ffff8265.w
        move.l  $70.w,old_vbl
        move.l  #vbl_handler,$70.w
        move.l  $120.w,old_timer_b
        move.l  #timer_b_handler,$120.w
        clr.b   $fffffa1b.w
        bclr    #0,$fffffa0b.w
        bclr    #0,$fffffa0f.w
        bset    #0,$fffffa07.w
        bset    #0,$fffffa13.w
        move.w  (sp)+,sr
        rts

uninstall:
        move.w  sr,-(sp)
        move.w  #$2700,sr
        bclr    #0,$fffffa13.w
        bclr    #0,$fffffa07.w
        clr.b   $fffffa1b.w
        move.l  old_timer_b,$120.w
        move.l  old_vbl,$70.w
        move.b  old_sync,$ffff820a.w
        clr.l   display_palette
        clr.l   pending_screen
        move.w  (sp)+,sr
        rts

; --- VBL vector hook -------------------------------------------------------
vbl_handler:
        movem.l d0-d1/a0-a1,-(sp)
        move.l  pending_screen,d0
        beq.s   .no_present
        move.l  d0,current_screen
        move.l  pending_palette,display_palette
        clr.l   pending_screen
        move.l  d0,d1
        swap    d1
        move.b  d1,$ffff8201.w
        move.b  d1,$ffff8205.w
        move.l  d0,d1
        lsr.w   #8,d1
        move.b  d1,$ffff8203.w
        move.b  d1,$ffff8207.w
        move.b  d0,$ffff820d.w
        move.b  d0,$ffff8209.w
.no_present:
        move.l  display_palette,d0
        beq.s   .done
        move.l  d0,a0
        lea     $ffff8240.w,a1
        rept 8
        move.l  (a0)+,(a1)+
        endr
        move.l  current_screen,d0
        add.l   #160,d0
        move.l  d0,line1_address
        clr.b   $fffffa1b.w
        bclr    #0,$fffffa0b.w
        move.b  #1,$fffffa21.w
        move.b  #8,$fffffa1b.w
.done:
        movem.l (sp)+,d0-d1/a0-a1
        move.l  old_vbl,-(sp)
        rts

; --- Timer B handler: the beam-timed frame ---------------------------------
timer_b_handler:
        move.w  #$2700,sr
        movem.l d0-d7/a0-a6,-(sp)
        clr.b   $fffffa1b.w
        move.l  display_palette,d0
        beq     tb_exit
        move.l  d0,a3
        move.l  line1_address,d4
        moveq   #0,d3
        moveq   #64,d5
        move.w  #LINES,d7
        subq.w  #3,d7
        bmi     tb_late
.find_line:
        moveq   #0,d0
        move.b  $ffff8205.w,d0
        lsl.w   #8,d0
        move.b  $ffff8207.w,d0
        lsl.l   #8,d0
        move.b  $ffff8209.w,d0
        sub.l   d4,d0
        bpl.s   .line_reached
        subq.w  #1,d5
        bne.s   .find_line
        bra     tb_late
.line_reached:
        cmpi.l  #12,d0
        bls.s   .sync_line
        add.l   #160,d4
        addq.w  #1,d3
        subq.w  #1,d7
        bmi     tb_late
        cmpi.w  #8,d3
        bhi     tb_late
        bra.s   .find_line
.sync_line:
        move.b  d4,d1
        tst.w   d3
        beq.s   .ready
        move.w  d3,d0
        mulu.w  #96,d0
        add.l   d0,a3
.ready:
        lea     32(a3),a3       ; skip the 16-word VBL palette (already set)
        lea     $ffff8240.w,a4
        lea     $ffff8209.w,a5
        moveq   #0,d0
        btst    #1,$ffff820a.w
        beq     tb_60hz
        move.w  #160,d6
        raster_frame 4,1
tb_60hz:
        move.w  #157,d6
        raster_frame 3,0
tb_done:
        addq.l  #1,raster_frames
        bra.s   tb_exit
tb_late:
        addq.l  #1,raster_skipped
tb_exit:
        movem.l (sp)+,d0-d7/a0-a6
        bclr    #0,$fffffa0f.w
        rte


        data
        even
mouse_off:  dc.b 18
mouse_on:   dc.b 8
        even
display_palette:  dc.l 0
pending_palette:  dc.l 0
pending_screen:   dc.l 0
current_screen:   dc.l 0
line1_address:    dc.l 0
screen_base:      dc.l 0
raster_frames:    dc.l 0
raster_skipped:   dc.l 0
old_vbl:          dc.l 0
old_timer_b:      dc.l 0
old_physbase:     dc.l 0
old_logbase:      dc.l 0
old_resolution:   dc.w 0
old_sync:         dc.b 0
        even
schedule:   incbin "timerb-schedule.bin"
        even

        bss
        even
screen_storage:   ds.b 32000+256
        even
        ds.l 512
stack_top:
        end
