#pragma once

namespace KWin {
    class Output;

    // RenderView is a Wayland-only thing
    // for our purposes (using the pointer for comparison/indexing)
    // Output fills roughly the same role
    using RenderView = Output;
}
