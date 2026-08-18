#include "../src/ui/grid_navigation.h"

#include <cassert>

int main() {
    assert(lunar::ui::gridTargetColumn(0, 4) == 0);
    assert(lunar::ui::gridTargetColumn(2, 4) == 2);
    assert(lunar::ui::gridTargetColumn(3, 2) == 1);
    assert(lunar::ui::gridTargetColumn(7, 1) == 0);
    assert(lunar::ui::gridTargetColumn(2, 0) == 0);
    return 0;
}
