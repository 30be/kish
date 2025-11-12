Write a file, kish.cpp using c++20, that will run a bash process, and fully simulate it - every hotkey(like ctrl+r or something) should be sent there, and every output should be sent from it - this includes the prompt. Also add the input and the output to the history.
BUT if the user presses Ctrl+x, then output "KI|" and then everything that user enters does not go to bash. It just goes to the history. When the user presses Ctrl+x again, print "|SH", call send_prompt(history) - make it a placeholder, which will just output the history.

The history should be identical to what the user sees.

Also implement backspaces, and all the hotkeys that could be sent to bash in general.

compile with clang++ kish.cpp -o kish (add c++ version)
test with ./kish <<< ls (for example)

So the whole program should be indistinguishable from bash until user presses ctrl+x.

Use std extensively, you can use other libraries too, but it is not encouraged(but allowed)
