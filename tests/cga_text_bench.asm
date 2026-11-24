; Disposable 360 KiB benchmark floppy. In 36 BIOS timer ticks, repeat:
; position cursor, write an 80-character row, scroll the full screen once.
; Boot the floppy in the guest. Sector 9 returns CD AB, two little-endian dwords (batches and ticks),
; then a 32-bit sum over the 2000 visible character/attribute pairs.
bits 16
org 7c00h
xor ax,ax
mov ds,ax
mov es,ax
mov ss,ax
mov sp,7800h
sti
mov ax,3
int 10h
xor eax,eax
mov [iterations],eax
mov eax,[46ch]
.wait_tick:
cmp eax,[46ch]
je .wait_tick
mov eax,[46ch]
mov [started],eax
.loop:
mov ah,02h
xor bx,bx
mov dx,0100h
int 10h
mov ax,0941h
mov bx,07h
mov cx,80
int 10h
mov ax,0601h
mov bh,07h
xor cx,cx
mov dx,184fh
int 10h
inc dword [iterations]
mov eax,[46ch]
sub eax,[started]
cmp eax,36
jb .loop
mov [ticks],eax
mov ax,0xb800
mov es,ax
xor di,di
xor eax,eax
mov cx,2000
.sum:
movzx edx,word [es:di]
add eax,edx
add di,2
loop .sum
mov [screen_sum],eax
xor ax,ax
mov es,ax
mov bx,result
mov cx,9
xor dx,dx
mov ax,0301h
int 13h
mov ax,0201h
int 13h
jmp $
result dw 0xabcd
iterations dd 0
ticks dd 0
screen_sum dd 0
started dd 0
times 510-($-$$) db 0
dw 0xaa55
times 360*1024-($-$$) db 0
