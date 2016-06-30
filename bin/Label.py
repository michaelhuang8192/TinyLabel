from _Label import *

import csv

cur_tmpl = None
cur_data = []
cur_nb_per_page = 1

def load_data(fnz, tmpl, flag):
    global cur_tmpl, cur_data, cur_nb_per_page

    if(flag):
        if flag & 0x01:
            cur_data = []
            cr = csv.reader(open(fnz, "rb"))
            hdr = cr.next()
            if hdr:
                for row in cr:
                    cur_data.append( dict( zip( hdr, row) ) )
        
        if flag & 0x02:
            tmpl_nz = "tmpl_%s.py" % (tmpl,)
            src = open("tmpl\\" + tmpl_nz, "rb").read()
            cur_tmpl = compile(src, tmpl_nz, "exec")
            
            cur_nb_per_page = 1
            tidx = tmpl.find('_')
            if tidx < 0: tidx = len(tmpl)
            sz = tmpl[:tidx].split('x')
            if len(sz) == 2 and sz[0].isdigit() and sz[1].isdigit():
                cur_nb_per_page = int(sz[0]) * int(sz[1])
        
    return (len(cur_data) + cur_nb_per_page - 1) / cur_nb_per_page


def draw_page(page_nb, screen_printing):
    if not cur_tmpl: return
    
    print cur_data
    idx = (page_nb - 1) * cur_nb_per_page
    cur_page_data = cur_data[idx:idx + cur_nb_per_page]
    exec(cur_tmpl, {
        'cur_page_nb': page_nb, 
        'cur_page_data': cur_page_data, 
        'screen_printing': screen_printing
        }
    )


