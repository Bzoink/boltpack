; Bounds-checked BoltLite decoder for Open Watcom's 16-bit cdecl ABI.
; Copyright (C) 2026 Tony Mattsson. BSD 2-Clause License.

.8086
ifdef BOLTLITE_FAR_CODE
.model large
ARG_START equ 6                 ; saved BP and far return address
else
.model compact
ARG_START equ 4                 ; saved BP and near return address
endif
.code

OUTPUT_START equ -12
PREVIOUS_DISTANCE equ -14

public _boltlite_decode_8086
ifdef BOLTLITE_FAR_CODE
_boltlite_decode_8086 proc far
else
_boltlite_decode_8086 proc near
endif
    push bp
    mov  bp,sp
    push di
    push si
    push bx
    push ds
    push es
    les  di,[bp+ARG_START+6]    ; far destination
    push di                     ; OUTPUT_START
    xor  ax,ax
    push ax                     ; PREVIOUS_DISTANCE
    lds  si,[bp+ARG_START]      ; far source
    mov  cx,[bp+ARG_START+4]    ; input remaining
    mov  dx,[bp+ARG_START+10]   ; output remaining
    cld

read_flags:
    cmp  cx,2                   ; group header and at least one token
    jb   invalid
    lodsb
    dec  cx
    mov  bl,al
    mov  bh,1                   ; sentinel follows the eight flag bits
read_token:
    shr  bx,1
    jz   read_flags
    jnc  match_token
    jcxz invalid
    movsb
    dec  cx
    dec  dx
    jnz  read_token

finished:
    jcxz valid
invalid:
    xor  ax,ax
    jmp  short restore
valid:
    mov  ax,1
restore:
    add  sp,4
    pop  es
    pop  ds
    pop  bx
    pop  si
    pop  di
    pop  bp
    ret

match_token:
    jcxz invalid
    lodsb
    dec  cx
    xor  ah,ah
    test al,80h
    jnz  long_distance
    test al,40h
    jnz  repeat_distance

packed_distance:
    push bx
    mov  bx,ax
    shr  bx,1
    shr  bx,1
    shr  bx,1
    inc  bx
    mov  [bp+PREVIOUS_DISTANCE],bx
    and  ax,7
    jmp  short copy_match

long_distance:
    jcxz invalid
    mov  ah,al
    and  ah,7fh
    lodsb
    dec  cx
    jmp  short packed_distance

repeat_distance:
    push bx
    mov  bx,[bp+PREVIOUS_DISTANCE]
    or   bx,bx
    jz   invalid_match
    and  ax,3fh
copy_match:
    add  ax,3
    cmp  dx,ax
    jb   invalid_match
    push si
    mov  si,di
    sub  si,[bp+OUTPUT_START]
    cmp  bx,si                  ; distance cannot precede output history
    ja   invalid_reference
    sub  dx,ax
    mov  si,di
    sub  si,bx
    push ds
    push es
    pop  ds                     ; history and output use the same segment
    push cx
    mov  cx,ax
    cmp  bx,1
    je   fill_run
    shr  cx,1
    rep  movsw                  ; each word is safe at distances of two or more
    adc  cx,cx                  ; MOVSW preserves carry (the residual byte)
    rep  movsb
    jmp  short match_done
fill_run:
    lodsb
    rep  stosb
match_done:
    pop  cx
    pop  ds
    pop  si
    pop  bx
    or   dx,dx
    jz   finished
    jmp  read_token

invalid_reference:
    pop  si
invalid_match:
    pop  bx
    jmp  invalid

_boltlite_decode_8086 endp
end
