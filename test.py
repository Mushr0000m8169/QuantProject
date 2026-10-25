from futu import *
import pandas as pd

quote_ctx = OpenQuoteContext(host='127.0.0.1', port=11111)

# 1. 創建一個空列表，用來存放每一頁的 DataFrame
df_list = []

# 請求第一頁數據 (代碼已修正為期貨主連 HK.HSImain)
ret, data, page_req_key = quote_ctx.request_history_kline(
    'HK.HSImain', start='2025-07-31', end='2026-10-13',
    ktype=KLType.K_DAY, max_count=1000, session=Session.ALL
)

if ret == RET_OK:
    df_list.append(data)  # 將第一頁的 DataFrame 存入列表
else:
    print('error:', data)

# 循環請求翻頁數據
while page_req_key is not None:
    print('*************************************')
    ret, data, page_req_key = quote_ctx.request_history_kline(
        'HK.HSImain', start='2025-07-31', end='2026-10-13',
        ktype=KLType.K_DAY, max_count=1000, page_req_key=page_req_key, session=Session.ALL
    )
    if ret == RET_OK:
        df_list.append(data)  # 將後續每一頁的 DataFrame 存入列表
    else:
        print('error:', data)

print('All pages fetched from Futu API!')
quote_ctx.close()

# ==================== PANDAS 數據處理與導出 ====================
if df_list:
    # 2. 使用 pd.concat 一次性高效合併所有分頁數據
    final_df = pd.concat(df_list, ignore_index=True)
    
    # 3. [可選] 如果只想保留 OHLC 核心欄位，可以用 Pandas 直接篩選
    final_df = final_df[['code', 'time_key', 'open', 'high', 'low', 'close', 'volume', 'turnover']]
    
    # 4. 一鍵導出為 CSV
    csv_filename = "HSI.csv"
    final_df.to_csv(csv_filename, index=False, encoding='utf-8-sig')
    
    print(f"Successfully saved {len(final_df)} rows to {csv_filename}!")
    print(final_df.head()) # 打印前 5 行預覽
else:
    print("No data collected.")
