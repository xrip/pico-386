; Build a disposable 360 KiB test floppy with nasm -f bin.
; Sector 9 returns CD AB on success or AD DE on failure, followed at +4
; by the video mode and test stage. Glyph checks read guest VRAM directly.
bits 16
org 0x7c00
xor ax,ax
mov es,ax
mov bx,0x8000
mov ax,0x0205
mov cx,3
xor dx,dx
int 0x13
jc $
jmp 0:0x8000
times 510-($-$$) db 0
dw 0xaa55

times 1024-($-$$) db 0
start:
    xor ax,ax
    mov ds,ax
    mov es,ax
    mov ss,ax
    mov sp,0x7800
    ; Verify BIOS ID, ROM size/checksum, guest INT 10h entry, VGA ports and tables.
    mov byte [0x605],0x80
    mov ax,1a00h
    int 10h
    cmp al,1ah
    jne fail
    cmp bx,8
    jne fail
    mov ax,1200h
    mov bl,10h
    int 10h
    cmp bx,3
    jne fail
    int 11h
    test al,30h
    jnz fail
    cmp word [0x42],0xc000
    jne fail
    mov ax,0xc000
    mov es,ax
    cmp word [es:0],0xaa55
    jne fail
    movzx cx,byte [es:2]
    shl cx,9
    xor ax,ax
    xor di,di
.sum:
    add al,[es:di]
    inc di
    loop .sum
    test al,al
    jnz fail
    mov dx,0x3cc
    in al,dx
    cmp al,0xff
    je fail
    mov bl,al
    mov dx,0x3c2
    xor al,4
    out dx,al
    mov ah,al
    mov dx,0x3cc
    in al,dx
    cmp al,ah
    jne fail
    mov dx,0x3c2
    mov al,bl
    out dx,al
    xor ax,ax
    mov es,ax
    mov di,0x680
    mov ax,1b00h
    xor bx,bx
    int 10h
    cmp al,1bh
    jne fail
    cmp byte [di+0x25],8
    jne fail
    cmp byte [di+0x31],3
    jne fail
    mov si,modes
nextmode:
    lodsb
    test al,al
    jz finish
    mov [0x604],al
    xor ah,ah
    int 10h
    mov byte [0x605],1
    mov ah,0fh
    int 10h
    cmp al,[0x604]
    jne fail
    cmp ah,[0x44a]
    jne fail
    mov byte [0x605],2
    mov bx,1
    mov ax,0941h
    mov cx,2
    int 10h
    cmp word [0x450],0
    jne fail
    call check_glyphs

    ; A BIOS cursor call must also work on the extended-mode surface.
    mov byte [0x605],3
    mov ax,0920h          ; leave row zero blank so a failed scroll is visible
    mov cx,2
    int 10h
    mov ax,0200h
    mov dx,0100h
    int 10h
    mov ax,0300h
    int 10h
    cmp dx,0100h
    jne fail
    mov ax,0a41h
    mov cx,2
    int 10h
    mov ax,0200h
    mov dh,[0x484]
    mov dl,[0x44a]
    dec dl
    int 10h
    mov ax,0e20h
    int 10h
    cmp byte [0x450],0
    jne fail
    mov al,[0x484]
    cmp [0x451],al
    jne fail
    call check_glyphs
    ; Pixel XOR, string attributes and off-screen pages in a planar VGA mode.
    cmp byte [0x604],0dh
    jne .next
    mov ax,1303h
    mov bx,0100h
    xor dx,dx
    mov cx,1
    mov bp,sample_text
    int 10h
    mov ax,0800h
    int 10h                ; cursor on page 1 moved one column
    mov ax,0200h
    xor dx,dx
    int 10h
    mov ax,0800h
    int 10h
    cmp al,'A'
    jne fail
    mov ax,0c04h
    xor cx,cx
    mov dx,70
    int 10h
    mov ax,0c81h
    int 10h
    mov ah,0dh
    int 10h
    cmp al,5
    jne fail
    xor bh,bh
    mov ah,0dh
    int 10h
    test al,al
    jnz fail
.next:
    jmp nextmode

; The first 16 pixels of each scanline must contain two copies of A.
; Read guest video memory directly, independent of the BIOS under test.
check_glyphs:
    push es
    mov ax,0xa000
    mov es,ax
    push es
    mov ax,1130h
    mov bh,1
    int 10h
    mov [fontseg],es
    mov [fontoff],bp
    pop es
    xor dx,dx
.row:
    mov ax,65
    mul byte [0x485]
    add ax,dx
    add ax,[fontoff]
    mov di,ax
    push ds
    mov ds,[fontseg]
    mov bh,[di]
    pop ds
    xor cx,cx
.pixel:
    mov di,dx
    mov al,[0x604]
    cmp al,13h
    je .linear
    cmp al,0ah
    jbe .tandy
    imul di,80
    cmp al,0dh
    jne .planes
    shr di,1
.planes:
    mov ax,cx
    shr ax,3
    add di,ax
    mov al,[es:di]        ; read plane zero (foreground 1)
    push cx
    and cl,7
    rol al,cl
    rol al,1
    pop cx
    and al,1
    jmp .check
.linear:
    imul di,320
    add di,cx
    mov al,[es:di]
    jmp .check
.tandy:
    cmp al,0ah
    jne .banks
    imul di,320
    jmp .nibble
.banks:
    mov ax,di
    and ax,3
    shr di,2
    imul di,160
    cmp byte [0x604],9
    je .bank_offset
    mov di,dx
    shr di,1
    imul di,80
    and ax,1
.bank_offset:
    shl ax,13
    add di,ax
    add di,8000h
.nibble:
    mov ax,cx
    shr ax,1
    add di,ax
    mov al,[es:di]
    test cl,1
    jnz .low
    shr al,4
.low:
    and al,15
.check:
    rol bh,1
    mov bl,bh
    and bl,1
    cmp al,bl
    jne fail
    inc cx
    cmp cx,16
    jb .pixel
    inc dx
    cmp dl,[0x485]
    jb .row
    pop es
    mov bx,1
    ret

finish:
    mov ax,3
    int 10h
    mov ax,0e51h
    int 10h
    mov ax,0xb800
    mov es,ax
    cmp byte [es:0],'Q'
    jne fail
    ; Text-mode positioning, character output and window scrolling.
    xor bx,bx
    mov dx,0100h
    mov ah,2
    int 10h
    mov ax,0952h
    mov bl,7
    mov cx,1
    int 10h
    mov ax,0601h
    mov bh,7
    xor cx,cx
    mov dx,014fh
    int 10h
    cmp word [es:0],0752h
    jne fail
    ; A two-entry DAC block ending at index 255 round-trips in RGB order.
    xor ax,ax
    mov es,ax
    mov ax,1012h
    mov bx,254
    mov cx,2
    mov dx,dac_values
    int 10h
    mov ax,1017h
    mov dx,dac_copy
    int 10h
    mov si,dac_values
    mov di,dac_copy
    mov cx,6
    repe cmpsb
    jne fail
    ; An unsupported mode probe must keep the current screen and mode.
    mov ax,0030h
    int 10h
    mov ah,0fh
    int 10h
    cmp al,3
    jne fail
    mov byte [0x605],0x90
    ; A DOS TSR must see the call first and be able to chain to the video ROM.
    cli
    mov ax,[0x40]
    mov [chain],ax
    mov ax,[0x42]
    mov [chain+2],ax
    mov word [0x40],chained
    mov word [0x42],0
    sti
    mov ax,1a00h
    int 10h
    cmp byte [hook_seen],1
    jne fail
    cmp al,1ah
    jne fail
    cli
    mov eax,[chain]
    mov [0x40],eax
    sti
    mov byte [0x605],0x92
    ; Save BIOS and DAC state, change a color, and restore.
    mov ax,1c00h
    mov cx,6
    int 10h
    cmp al,1ch
    jne fail
    cmp bx,13
    jne fail
    xor ax,ax
    mov es,ax
    mov bx,0x9000
    mov ax,1c01h
    int 10h
    mov ax,1010h
    mov bx,254
    xor cx,cx
    xor dx,dx
    int 10h
    mov ax,1c02h
    mov bx,0x9000
    mov cx,6
    int 10h
    mov ax,1015h
    mov bx,254
    int 10h
    cmp dh,1
    jne fail
    cmp cx,0203h
    jne fail
    mov byte [0x605],0x93
    ; Write an attributed string to the inactive text page, then select it.
    mov ax,1303h
    mov bx,0100h
    mov cx,2
    mov dx,0100h
    mov bp,sample_text
    int 10h
    mov ax,0501h
    int 10h
    cmp word [0x44e],1000h
    jne fail
    mov ax,0200h
    mov bx,0100h
    mov dx,0101h
    int 10h
    mov ah,08h
    int 10h
    cmp ax,2f42h
    jne fail
    ; Select the resident 8x8 VGA text font, write below row 25, then restore.
    mov ax,1202h
    mov bl,30h
    int 10h
    mov ax,0003h
    int 10h
    mov ax,1112h
    xor bx,bx
    int 10h
    cmp word [0x485],8
    jne fail
    cmp byte [0x484],49
    jne fail
    mov ax,0200h
    mov dx,3000h
    int 10h
    mov ax,0958h
    mov bx,7
    mov cx,1
    int 10h
    mov ah,08h
    int 10h
    cmp ax,0758h
    jne fail
    mov ax,1114h
    int 10h
    cmp byte [0x484],24
    jne fail
    mov byte [0x605],0x91
    ; Check packed CGA pixels and glyph readback in modes 4, 5 and 6.
    mov si,cgamodes
.cgamode:
    lodsb
    test al,al
    jz .done
    xor ah,ah
    int 10h
    mov ax,0941h
    mov bx,1
    mov cx,1
    int 10h
    mov ah,08h
    int 10h
    cmp al,'A'
    jne fail
    mov ax,0c01h
    mov cx,15
    mov dx,1
    int 10h
    mov ah,0dh
    int 10h
    cmp al,1
    jne fail
    mov ax,0c81h
    int 10h
    mov ah,0dh
    int 10h
    test al,al
    jnz fail
    jmp .cgamode
.done:
    ; Mode 7 writes the monochrome aperture. Clearing CGA modes 4..6
    ; must erase their graphics banks, while AL bit 7 keeps the pixels.
    mov ax,7
    int 10h
    mov ax,095ah
    mov bx,7
    mov cx,1
    int 10h
    mov ax,0xb000
    mov es,ax
    cmp word [es:0],075ah
    jne fail
    mov ax,4
    int 10h
    mov ax,0c02h
    mov cx,19
    mov dx,9
    int 10h
    mov ax,0084h
    int 10h
    mov ah,0dh
    int 10h
    cmp al,2
    jne fail
    mov ax,4
    int 10h
    mov ah,0dh
    int 10h
    test al,al
    jnz fail
    mov word [0x600],0xabcd
    jmp report
fail:
    mov word [0x600],0xdead
report:
    xor ax,ax
    mov es,ax
    mov bx,0x0600
    mov ax,0301h
    mov cx,9
    xor dx,dx
    int 13h
    mov ax,0201h
    int 13h
    jmp $
modes db 8,9,10,0dh,0eh,10h,11h,12h,13h,0
chained:
    inc byte [cs:hook_seen]
    jmp far [cs:chain]
hook_seen db 0
chain dd 0
cgamodes db 4,5,6,0
sample_text db 'A',0x1e,'B',0x2f
fontseg dw 0
fontoff dw 0
dac_values db 1,2,3,4,5,6
dac_copy times 6 db 0

times 360*1024-($-$$) db 0
