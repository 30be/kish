# KISH

Agentic shell that does not get in the way. No fancy UI, no bullshit checks: you only live once!
Just say what you want, give access to the terminal and enjoy.

## Name

KI stands for Künstliche Intelligenz, which means Artificial Intelligence in German.
SH stands for shell.

## Install

```sh
git clone https://github.com/30be/kish.git
cd kish
clang++ -std=c++20 kish.cpp -o kish && ./kish # Or g++
sudo cp kish /usr/bin/kish
chsh -s /usr/bin/kish
```

You are done. Set the MOONSHOT_API_KEY environment variable - either in ~/.env or by hand.

You can get it [here](https://platform.moonshot.ai/console/api-keys).

## Usage

It is just bash, until you press Ctrl+x.
It will look like that:

```sh
~ $ KI|
```

Now you can type anything, let's say

```sh
~ $ KI|Write compile and run helloworld.cpp
```

Now just press Ctrl+x again, and wait:

```sh
~ $ KI|Write compile and run helloworld.cpp|SH
~ $ echo '#include <iostream>
>
> int main() {
>     std::cout << "Hello, World!" << std::endl;
>     return 0;
> }' > helloworld.cpp # Create helloworld.cpp with Hello World program
~ $ clang++ helloworld.cpp -o helloworld # Compile it
~ $ ./helloworld # Run the executable
Hello World
~ $
The hello world program has been successfully written, compiled, and executed. The output "Hello World" confirms it ran correctly.
```

\*It does not actually work like that every time now, but i am working towards it.

## Notes

YOLO mode is conveniently enabled by default(nothing else is implemented)

To know what kish thinks about you, you can export the history with it's thoughts using the ctrl+h shortcut.
