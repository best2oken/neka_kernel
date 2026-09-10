; void context_switch(uint32_t *old_esp_ptr, uint32_t new_esp)
; Тот же принцип, что в EKA2 ncsched.cia:
;   mov [esi+iSavedSP], esp   -> сохранить стек старого потока
;   mov esp, [ebx+iSavedSP]   -> загрузить стек нового потока
; Разница только в том, что там это часть Reschedule() с поиском потока,
; здесь — чистый примитив свитча, вызываемый уже после того как sched.c
; выбрал next/prev.

section .text
global context_switch

context_switch:
    push ebp
    mov ebp, esp

    ; Сохраняем регистры вызывающего (аналог push fs/gs/ebp/edi/esi/ebx в EKA2)
    push ebx
    push esi
    push edi
    push ebp

    mov eax, [ebp+8]      ; old_esp_ptr
    mov [eax], esp         ; *old_esp_ptr = esp   (сохранить текущий стек)

    mov esp, [ebp+12]      ; esp = new_esp        (переключиться на новый стек)

    pop ebp
    pop edi
    pop esi
    pop ebx

    pop ebp
    ret
