import io, sys

p = 'tests/test_acp_alignment.cc'
block_path = 'tests/.bgtask_block.inc'

s = io.open(p, encoding='utf-8').read()
block = io.open(block_path, encoding='utf-8').read()

anchor = '''    // =======================================================================
    std::cout << "\\n========================================\\n";'''

n = s.count(anchor)
if n != 1:
    print('ANCHOR COUNT', n)
    sys.exit(1)

s = s.replace(anchor, block + anchor)

inc_anchor = '#include "hunterclaw/acp/acp_translator.h"'
if s.count(inc_anchor) != 1:
    print('INC ANCHOR COUNT', s.count(inc_anchor))
    sys.exit(1)

s = s.replace(
    inc_anchor,
    inc_anchor
    + '\n#include "hunterclaw/acp/acp_background_task.h"'
    + '\n#include "hunterclaw/tasks/task_completion_contract.h"',
)

io.open(p, 'w', encoding='utf-8').write(s)
print('OK inserted')
