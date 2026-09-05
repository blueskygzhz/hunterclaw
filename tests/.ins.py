import io, sys

p = 'tests/test_acp_alignment.cc'
s = io.open(p, encoding='utf-8').read()
block = io.open('tests/.blk.inc', encoding='utf-8').read()

anchor = '''    // =======================================================================
    std::cout << "\\n========================================\\n";'''
if s.count(anchor) != 1:
    print('ANCHOR', s.count(anchor)); sys.exit(1)
s = s.replace(anchor, block + anchor)

inc = '#include "hunterclaw/acp/acp_background_task.h"'
if s.count(inc) != 1:
    print('INC', s.count(inc)); sys.exit(1)
s = s.replace(inc, inc
    + '\n#include "hunterclaw/acp/acp_turn_timeout.h"'
    + '\n#include "hunterclaw/acp/acp_backend_failover.h"'
    + '\n#include "hunterclaw/acp/acp_active_turns.h"')

for hdr in ('<thread>', '<chrono>', '<memory>'):
    if '#include ' + hdr not in s:
        s = s.replace(inc, '#include ' + hdr + '\n' + inc, 1)

io.open(p, 'w', encoding='utf-8').write(s)
print('OK')
