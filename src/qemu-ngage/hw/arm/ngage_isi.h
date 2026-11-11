/* N-Gage (EKA1) board: NOSDRIVER channels / ISI transport (ngage_isi.c). */
#ifndef NGAGE_ISI_H
#define NGAGE_ISI_H

bool ngage_isi_desc(uint32_t va, uint8_t *buf, uint32_t cap, uint32_t *len, uint32_t *max,
                    uint32_t *data);
void ngage_isi_ctor(uint32_t obj, uint32_t unit);
void ngage_isi_open(uint32_t dlc, uint32_t unit);
void ngage_isi_call(uint32_t obj, unsigned method, uint32_t r1, uint32_t a1, uint32_t a2);
void ngage_isi_load(const char *path);
bool ngage_isi_ready(void);
bool ngage_isi_fetch(uint32_t *dlc, uint32_t *a1, uint32_t *req, uint32_t *len);
uint32_t ngage_isi_fetched_word(unsigned off);
void ngage_isi_done(int32_t result);

#endif
