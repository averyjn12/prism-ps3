#!/bin/bash
# v0.67: planted bugs in a copy of src/played.c, against host/played_test.c. Caught = the test's own FAIL line.
HERE="$(cd "$(dirname "$0")" && pwd)"
B="$HOME/prism_app_host/played_mut"
mkdir -p "$B"
gcc -O2 -w -I"$HERE/../src" "$HERE/played_test.c" "$HERE/../src/played.c" -o "$B/t" && "$B/t" "$B/p.txt" > "$B/base.log" || { echo "BROKEN: the unmutated test fails"; exit 2; }
caught=0; total=0
mut() {   # name, sed expression
    total=$((total + 1))
    sed "$2" "$HERE/../src/played.c" > "$B/played.c"
    cmp -s "$B/played.c" "$HERE/../src/played.c" && { echo "BROKEN: $1: nothing changed"; exit 2; }
    gcc -O2 -w -I"$HERE/../src" "$HERE/played_test.c" "$B/played.c" -o "$B/t" || { echo "BROKEN: $1 does not compile"; exit 2; }
    timeout 20 "$B/t" "$B/p.txt" > "$B/run.log" 2>&1
    if grep -q "^FAIL:" "$B/run.log"; then caught=$((caught + 1)); echo "caught   $1 ($(grep -m1 "^FAIL:" "$B/run.log" | cut -c1-70))"; else echo "SURVIVED $1"; fi
}
mut "count not raised" 's/^\te.count++;$/\t;/'
mut "list order reversed on load" 's/return x->seq < y->seq ? 1 : x->seq > y->seq ? -1 : 0;/return x->seq < y->seq ? -1 : x->seq > y->seq ? 1 : 0;/'
mut "undo not saved" 's/^\tn_before = -1;\n\tsave();$/XX/; /^void played_undo/,/^}/ s/^\tsave();$/\t;/'
mut "full list drops the newest" 's/at = PLAYED_MAX - 1;/at = 0;/'
mut "clock not stored" 's/^\te.last = now;$/\t;/'
mut "platform ignored in find" 's/if (list\[i\].plat == plat \&\& !strcmp(list\[i\].name, name))\n\t\t\treturn/XX/; /^const played_t \*played_find/,/^}/ s/list\[i\].plat == plat \&\& //'
mut "zero count lines accepted" 's/ || p > 255 || !c)/ || p > 255)/'
mut "heading not checked" 's/if (fgets(ln, sizeof ln, f) \&\& !strncmp(ln, HEAD, strlen(HEAD)))/if (fgets(ln, sizeof ln, f))/'
mut "tab in a name accepted" 's/strpbrk(name, "[^"]*")/strpbrk(name, "X")/'
mut "order number never rises" 's/e.seq = n_before > 0 ? list\[0\].seq + 1 : 1;/e.seq = 1;/'
mut "minutes shown as seconds" 's/(unsigned)(d \/ 60));/(unsigned)d);/'
echo "played_mutants: $caught of $total caught"
[ "$caught" = "$total" ]
