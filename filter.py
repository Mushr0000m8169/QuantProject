from futu import *
quote_ctx = OpenQuoteContext(host='127.0.0.1', port=11111)

simpleFilter = SimpleFilter()
simpleFilter.stock_field = StockField.MARKET_VAL
simpleFilter.filter_min = 30000000000
simpleFilter.is_no_filter = False

filter_lot_price = SimpleFilter()
filter_lot_price.stock_field = StockField.LOT_PRICE
filter_lot_price.filter_min = 2000
filter_lot_price.filter_max = 5500
filter_lot_price.is_no_filter = False


accFilter = AccumulateFilter()
accFilter.stock_field = StockField.AMPLITUDE
accFilter.filter_min = 3
accFilter.days = 5
accFilter.is_no_filter = False


stockCodeList = []
stockBaseInfo = {}

nBegin = 0
last_page = False
ret_list = list()
while not last_page:
    nBegin += len(ret_list)
    ret, ls = quote_ctx.get_stock_filter(market=Market.HK, filter_list=[simpleFilter, filter_lot_price, accFilter], begin=nBegin)  # 对香港市场的股票做简单、财务和指标筛选
    if ret == RET_OK:
        last_page, all_count, ret_list = ls
        print('all count = ', all_count)
        for item in ret_list:
            stockCodeList.append(item.stock_code)
            stockBaseInfo[item.stock_code] = {
                "股票名稱": item.stock_name,
                "每手金額(篩選時)": item[filter_lot_price]
            }
            print(f"{item.stock_code} {item.stock_name} {item[filter_lot_price]}")
    else:
        print('error: ', ls)
    time.sleep(3)  # 加入时间间隔，避免触发限频

df = pd.DataFrame(stockCodeList, columns = ['code'])
df.to_csv('monitorCodeList1.csv',index = False)


'''
flag ,df = quote_ctx.get_market_snapshot(stockCodeList)
if flag == RET_OK:
    print(df)
else:
    print("fail to get snapshot")
'''
    
'''
for target in stockCodeList:
    ret ,df,key = quote_ctx.request_history_kline(code = target,start = '2024-01-01', end = '2026-10-14',ktype = KLType.K_WEEK,max_count = 1000,session = Session.ALL)
    if ret == RET_OK:
        text = target
        text = text.replace(".","")
        filePath = f"w_history/{text}.csv"
        df.to_csv(filePath,index = False)
        time.sleep(3)
'''
quote_ctx.close()
