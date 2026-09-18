; Native STE preview of Monkey Island room 28 with a real DOS-CD COST scene.
; The top 144 scanlines use host-selected 16-register Spectrum512 frames with
; a batched world-space Byle-RLE costume frame; the lower 56 scanlines are a
; GEM-style panel.
FRAME_BYTES equ 51296
FRAME_COUNT equ 31
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
        move.w  #$4a,-(sp)
        trap    #1
        lea     12(sp),sp
        move.w  #4,-(sp)
        trap    #14
        addq.l  #2,sp
        move.w  d0,old_resolution
        cmpi.w  #2,d0
        beq     exit_program
        pea     check_ste
        move.w  #$26,-(sp)
        trap    #14
        addq.l  #6,sp
        tst.w   d0
        beq     exit_program
        move.w  #2,-(sp)
        trap    #14
        addq.l  #2,sp
        move.l  d0,old_physbase
        move.w  #3,-(sp)
        trap    #14
        addq.l  #2,sp
        move.l  d0,old_logbase
        pea     save_hardware
        move.w  #$26,-(sp)
        trap    #14
        addq.l  #6,sp

        lea     screen_storage,a0
        move.l  a0,d0
        addi.l  #255,d0
        andi.l  #$ffffff00,d0
        move.l  d0,front_screen
        addi.l  #32000,d0
        move.l  d0,back_screen
        clr.w   -(sp)
        move.l  front_screen,-(sp)
        move.l  front_screen,-(sp)
        move.w  #5,-(sp)
        trap    #14
        lea     12(sp),sp
        lea     frame_source,a0
        move.l  front_screen,a1
        move.l  back_screen,a2
        move.w  #(32000/4)-1,d7
.copy_screen:
        move.l  (a0)+,d0
        move.l  d0,(a1)+
        move.l  d0,(a2)+
        dbf     d7,.copy_screen
        move.l  #frame_source+32000,front_palettes
        move.l  #frame_source+32000,back_palettes
        pea     mouse_off
        clr.w   -(sp)
        move.w  #$19,-(sp)
        trap    #14
        addq.l  #8,sp
        pea     install_display
        move.w  #$26,-(sp)
        trap    #14
        addq.l  #6,sp
        move.w  #1,pending_view
.present:
        tst.w   pending_view
        bpl.s   .present
animation_loop:
        bsr     poll_input
        bsr     advance_frame
        move.w  #1,pending_view
.wait_frame:
        tst.w   pending_view
        bpl.s   .wait_frame
        bra.s   animation_loop

cleanup:
        pea     remove_display
        move.w  #$26,-(sp)
        trap    #14
        addq.l  #6,sp
        move.w  old_resolution,-(sp)
        move.l  old_physbase,-(sp)
        move.l  old_logbase,-(sp)
        move.w  #5,-(sp)
        trap    #14
        lea     12(sp),sp
        pea     restore_hardware
        move.w  #$26,-(sp)
        trap    #14
        addq.l  #6,sp
        pea     mouse_on
        clr.w   -(sp)
        move.w  #$19,-(sp)
        trap    #14
        addq.l  #8,sp
exit_program:
        clr.w   -(sp)
        trap    #1

poll_input:
        move.w  #2,-(sp)
        move.w  #1,-(sp)
        trap    #13
        addq.l  #4,sp
        tst.l   d0
        beq.s   .done
        move.w  #2,-(sp)
        move.w  #2,-(sp)
        trap    #13
        addq.l  #4,sp
        cmpi.b  #27,d0
        beq     cleanup
.done:
        rts

advance_frame:
        move.w  frame_index,d0
        add.w   frame_direction,d0
        cmpi.w  #(FRAME_COUNT-1),d0
        ble.s   .check_low
        move.w  #(FRAME_COUNT-2),d0
        move.w  #-1,frame_direction
.check_low:
        tst.w   d0
        bge.s   .store
        moveq   #1,d0
        move.w  #1,frame_direction
.store:
        move.w  d0,frame_index
        mulu.w  #FRAME_BYTES,d0
        move.l  #frame_source,a0
        add.l   d0,a0
        move.l  a0,a2
        move.l  back_screen,a1
        move.w  #(32000/4)-1,d7
.copy_frame:
        move.l  (a0)+,(a1)+
        dbf     d7,.copy_frame
        lea     32000(a2),a0
        move.l  a0,back_palettes
        rts

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
        bne.s   .no
        moveq   #1,d0
        rts
.no:
        moveq   #0,d0
        rts

save_hardware:
        move.b  $ffff820a.w,old_sync
        move.b  $ffff820d.w,old_base_low
        move.b  $ffff820f.w,old_line_width
        move.b  $ffff8265.w,old_scroll
        lea     $ffff8240.w,a0
        lea     old_palette,a1
        moveq   #7,d0
.save_palette:
        move.l  (a0)+,(a1)+
        dbf     d0,.save_palette
        rts

restore_hardware:
        move.b  old_sync,$ffff820a.w
        move.b  old_base_low,$ffff820d.w
        move.b  old_line_width,$ffff820f.w
        move.b  old_scroll,$ffff8265.w
        lea     old_palette,a0
        lea     $ffff8240.w,a1
        moveq   #7,d0
.restore_palette:
        move.l  (a0)+,(a1)+
        dbf     d0,.restore_palette
        rts

install_display:
        move.w  sr,-(sp)
        move.w  #$2700,sr
        clr.b   $ffff820a.w
        clr.b   $ffff820d.w
        clr.b   $ffff820f.w
        clr.b   $ffff8265.w
        move.l  #frame_source+32000,display_palettes_pointer
        move.w  #-1,pending_view
        move.l  $456.w,a0
        move.l  a0,vbl_queue
        move.l  (a0),old_vbl
        move.l  #spectrum_vbl,(a0)
        move.w  (sp)+,sr
        rts

remove_display:
        move.w  sr,-(sp)
        move.w  #$2700,sr
        move.l  vbl_queue,a0
        move.l  old_vbl,(a0)
        move.w  (sp)+,sr
        rts

        include "../raster.s"

        data
        even
mouse_off:  dc.b 18
mouse_on:   dc.b 8
        even
pending_view: dc.w -1
front_old_y: dc.w -1
back_old_y: dc.w -1
display_palettes_pointer: dc.l 0
vbl_counter: dc.l 0
front_screen: dc.l 0
back_screen: dc.l 0
front_palettes: dc.l 0
back_palettes: dc.l 0
vbl_queue: dc.l 0
old_vbl: dc.l 0
        even
frame_index: dc.w 0
frame_direction: dc.w 1
frame_source: incbin "monkey-bar/native-assets/monkey-frames.bin"

        bss
old_resolution: ds.w 1
old_sync: ds.b 1
old_base_low: ds.b 1
old_line_width: ds.b 1
old_scroll: ds.b 1
        even
old_physbase: ds.l 1
old_logbase: ds.l 1
old_palette: ds.w 16
screen_storage: ds.b 64000+255
        even
        ds.l 512
stack_top:
        end
