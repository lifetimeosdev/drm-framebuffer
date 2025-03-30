.section .data

.global _picture_start
.type _picture_start, @object
_picture_start:
.incbin "logo.dat"

.global _picture_end
.type _picture_end, @object
_picture_end:
