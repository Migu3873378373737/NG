#ifndef NG_WACORE_H
#define NG_WACORE_H
/* Registra o modulo nativo @wacore: cliente WhatsApp Web em C puro,
 * com credenciais persistentes, pareamento por QR, Signal (X3DH +
 * double ratchet), recebimento de mensagens e envio de texto/botoes.
 * Ver ng_wacore.c para a API NG exposta. */
void ng_wacore_module_register(void);
#endif
