# ToyC_Compiler

武汉大学计算机科学专业大二第三学期编译实践课程仓库

## 运行要求

(版本号是我的电脑上的，版本略低可能也能正常运行)：

cmake （4.3.4）

MinGW（g++ 6.3.0）

flex(2.6.4)

bison(3.8.2)

推荐使用msys2 下载后三个（保持一下统一，我已经快被奇奇怪怪的问题折磨疯了）

请自行上网搜索下载安装方法，检查方法为，打开windows powershell界面（win+R  输入”cmd“，回车），依次输入以下代码：

~~~
cmake --version
g++ --version
flex --version
bison --version
~~~

若正确显示版本号，说明安装成功且能找到路径。

### CMake的使用

此处不会介绍cmake的原理，简单讲，cmake通过读取CMakeLists.txt文件的内容来构建项目，我在使用vscode的cmake插件时出现问题，拼尽全力未能解决。遂采用控制台命令来生成。

我对这个cmake也不熟，大家一起学习，之后会慢慢完善的。

~~~powershell
Remove-Item -Recurse -Force build  #清理命令，将整个build文件夹删除，在修改了cmakelists文件后来一下

cmake -S . -B build -G "MinGW Makefiles" # 执行阶段，读取cmakelist，会生成一个build文件夹，生成makefile等文件，阿巴阿巴（我没搞那么懂）

cmake --build build # 构建阶段，根据刚刚生成的makefile文件，生成可执行文件。
~~~

### flex & bison

**Flex和Bison是一对经典的组合工具，用于自动生成词法分析器和语法分析器**，是构建编译器、解释器及其他语言处理应用的核心组件。

它们是Unix经典工具**Lex和Yacc**的现代化身：Flex是GNU版本的Lex，而Bison是GNU版本的Yacc。

##### 🔧 它们各自是什么？

Flex和Bison分工明确，分别处理编译过程中的两个核心阶段：

* **Flex (词法分析器生成器)**: 它的工作是将输入的**字符流**（如源代码）分解成一个个有意义的“**词法单元(Token)**”。你通过编写**正则表达式**来定义各种Token的模式，Flex会据此生成C代码，这个代码就是一个能够识别这些Token的函数（通常是`yylex()`）。
* **Bison (语法分析器生成器)**: 它接收Flex生成的**Token流**，并根据你定义的**语法规则**（通常使用**上下文无关文法**，如BNF范式），检查这些Token的组合是否符合语法。如果符合，Bison会执行相应的**语义动作**（也是一段C代码），比如构建**抽象语法树(AST)** 或直接计算结果。

##### 🤝 它们如何协同工作？

两者的协作是典型的“控制反转”模式，主要由Bison驱动：

1. **定义共享Token**：首先，你在Bison的语法文件（`.y`文件）中声明所有Token。运行Bison时加上 `-d` 选项，它会生成一个头文件（通常是 `*.tab.h`），其中包含了这些Token的宏定义。
2. **Flex包含头文件**：然后，你在Flex的词法文件（`.l`文件）的 `%{...%}` 部分，用 `#include` 指令包含Bison生成的头文件。这样Flex就能识别并返回这些Token了。
3. **Bison调用Flex**：编译时，Bison生成的语法分析器（包含`yyparse`函数）会不断调用Flex生成的词法分析器（`yylex`函数）来获取下一个Token。
4. **传递数据**：Flex识别到一个Token后，通过**返回值**告诉Bison是哪种Token（如`NUMBER`），并将Token的**具体值**（如数字`123`）存入一个名为`yylval`的全局变量中。Bison随后便可以使用这些数据执行语义动作。
