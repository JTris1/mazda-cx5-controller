#pragma once

// Prints the command list and the first prompt. Call at the end of setup().
void consoleBegin();

// Call every loop: reads serial input, echoes it, and runs a command on Enter.
void consolePoll();
