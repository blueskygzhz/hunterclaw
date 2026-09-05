import io, sys

p = 'tests/test_acp_alignment.cc'
s = io.open(p, encoding='utf-8').read()
block = io.open('tests/.timeout_block.inc', encoding='utf-8').read()

anchor = '''    // =======================================================================
    std::cout << "\\n========================================\\n";'''

if s.count(anchor) != 1:
    print('ANCHOR COUNT', s.count(anchor))
    sys.exit(1)

s = s.replace(anchor, block + anchor)

inc_anchor = '#include "hunterclaw/acp/acp_background_task.h"'
if s.count(inc_anchor) != 1:
    print('INC ANCHOR COUNT', s.count(inc_anchor))
    sys.exit(1)

s = s.replace(
    inc_anchor,
    inc_anchor + '\n#include "hunterclaw/acp/acp_turn_timeout.h"',
)

# 超时用例需要 sleep / steady_clock
for hdr in ('<thread>', '<chrono>'):
    if '#include ' + hdr not in s:
        s = s.replace('#include "hunterclaw/acp/acp_translator.h"',
                      '#include ' + hdr + '\n#include "hunterclaw/acp/acp_translator.h"', 1)

io.open(p, 'w', encoding='utf-8').write(s)
print('OK inserted')
