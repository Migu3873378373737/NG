#ifndef NG_ZAP_H
#define NG_ZAP_H
/* Registra o modulo nativo @zap: protocolo do WhatsApp Web em C puro.
 * Codec binario de nos (wabinary), handshake Noise_XX_25519_AESGCM_SHA256
 * e cifragem do transporte. Ver ng_zap.c para o mapa de funcoes. */
void ng_zap_module_register(void);
#endif
