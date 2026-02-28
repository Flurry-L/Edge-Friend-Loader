#include "dx.h"
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv)
{
	try {
		EdgefriendDX12 dx;
		dx.SetIters(1);

		bool checkMode = false;
		float epsilon = 2e-5f;
		for (int i = 1; i < argc; ++i) {
			const std::string arg = argv[i];
			if (arg == "--check") {
				checkMode = true;
			}
			else if (arg == "--eps") {
				if (i + 1 >= argc) {
					throw std::invalid_argument("Missing value after --eps.");
				}
				epsilon = std::stof(argv[++i]);
			}
		}

		if (checkMode) {
			const bool matched = dx.RunAndCompareWithCpu(epsilon);
			return matched ? 0 : 2;
		}

		dx.OnInit();
		return 0;
	}
	catch (const std::exception& ex) {
		std::cerr << "Error: " << ex.what() << '\n';
		return 1;
	}
}
