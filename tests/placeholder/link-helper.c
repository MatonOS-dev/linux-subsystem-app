/*
 * Minimal hardlink caller for tests/placeholder/run.sh.
 *
 * The fixture creates its deploy/metadata placeholders by linking a seed file
 * from inside the marked store, so the link runs under the real compatibility
 * proxy exactly as a program under the proxy would. Keeping it a separate tiny
 * binary avoids depending on a host `ln` implementation.
 */
#include <unistd.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    if (link(argv[1], argv[2]) != 0) { perror("link"); return 1; }
    return 0;
}
