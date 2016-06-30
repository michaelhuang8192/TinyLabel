import Label as lbx

W = 180
H = 50

X = 19
Y = 49

GapX = 15
GapY = 2

NumPerRow = 3

print cur_page_data
for i in range(len(cur_page_data)):
	item = cur_page_data[i]
	rowIdx, colIdx = divmod(i, NumPerRow)
	x = X + colIdx * (W + GapX)
	y = Y + rowIdx * (H + GapY)
	print x, y
	lbx.fill_rect( (x, y, W, H), 0xf9d8a8 )
	lbx.draw_text( unicode(item['Item Name']), 12,  (x + 10, y + 10, 0, 0), family=u"Verdana")
