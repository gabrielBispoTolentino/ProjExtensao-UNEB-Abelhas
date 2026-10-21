"""Transforma o .ino num .cpp compilável no PC, como a IDE do Arduino faz:
põe #include <Arduino.h> no topo e os protótipos de todas as funções antes
da primeira definição (o sketch usa funções antes de defini-las).

    python gen_sketch.py ColmeiaMonitor.ino sketch.cpp
"""
import re
import sys

source, target = sys.argv[1], sys.argv[2]
text = open(source, encoding='utf-8').read()

# Definições de função no nível de topo: começam na coluna 0, terminam em "{".
definition = re.compile(r'^(?!static_assert|return|else|if|for|while|switch|class|struct|namespace|#)'
                        r'([A-Za-z_][\w:<>&*\s]*?[\s*&])([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*\{', re.M)
prototypes = []
first = None
for m in definition.finditer(text):
    if first is None:
        first = m.start()
    prototypes.append('%s%s(%s);' % (m.group(1), m.group(2), m.group(3)))

out = ['#include <Arduino.h>', '#line 1 "%s"' % source.replace(chr(92), '/')]
out.append(text[:first])
out.append('// Protótipos gerados (gen_sketch.py)')
out.extend(prototypes)
out.append(text[first:])
open(target, 'w', encoding='utf-8').write('\n'.join(out))
print('%d prototypes' % len(prototypes))
