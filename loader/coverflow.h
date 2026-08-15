/*
 * coverflow.h - 純圖形的選單: 封面並排, 左右滑動
 *
 * 沒有文字。選哪一個由封面自己說明 —— 字型是這台機器上最貴的東西
 * (中文字模放不進 16KB,只能從 SD 卡讀,每個字一次存取),而封面已經夠清楚。
 * 錯誤與燒錄進度仍然是文字,那些沒有圖可以代替,而且沉默的失敗最難查。
 */
#ifndef LOADER_COVERFLOW_H
#define LOADER_COVERFLOW_H

#include "fatlite.h"

/* 換一批檔案(開機或重新掃卡)之後要呼叫,丟掉舊的快取 */
void cf_reset(void);

/* 靜止畫面: 把 cursor 那張畫在正中央 */
void cf_draw(const fl_entry_t *entries, int count, int cursor);

/*
 * 從 from 滑到 to(相鄰的兩格)。畫完之後畫面等同 cf_draw(.., to)。
 * 只重畫封面帶那條橫幅,不動整個畫面 —— 那是動畫跑得動的關鍵。
 */
void cf_slide(const fl_entry_t *entries, int count, int from, int to);

#endif /* LOADER_COVERFLOW_H */
