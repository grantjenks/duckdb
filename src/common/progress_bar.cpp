#include "duckdb/common/progress_bar.hpp"
#include "duckdb/common/printer.hpp"

namespace duckdb {

void ProgressBar::ProgressBarThread() {
#ifndef DUCKDB_NO_THREADS
	WaitFor(std::chrono::milliseconds(show_progress_after));
	while (!stop) {
		int new_percentage;
		supported = executor->GetPipelinesProgress(new_percentage);
		if (new_percentage > 100 || new_percentage < current_percentage) {
			valid_percentage = false;
		}
		current_percentage = new_percentage;
		if (supported && current_percentage > -1) {
			Printer::PrintProgress(current_percentage, PROGRESS_BAR_STRING.c_str(), PROGRESS_BAR_WIDTH);
		}
		WaitFor(std::chrono::milliseconds(time_update_bar));
	}
#endif
}

int ProgressBar::GetCurrentPercentage() {
	return current_percentage;
}

bool ProgressBar::IsPercentageValid() {
#ifndef DUCKDB_NO_THREADS
	return valid_percentage;
#else
	return true;
#endif
}

void ProgressBar::Start() {
#ifndef DUCKDB_NO_THREADS
	valid_percentage = true;
	current_percentage = 0;
	progress_bar_thread = std::thread(&ProgressBar::ProgressBarThread, this);
#endif
}

void ProgressBar::Stop() {
#ifndef DUCKDB_NO_THREADS
	if (progress_bar_thread.joinable()) {
		{
			std::lock_guard<std::mutex> l(m);
			stop = true;
		}
		c.notify_one();
		progress_bar_thread.join();
		if (supported) {
			Printer::FinishProgressBarPrint(PROGRESS_BAR_STRING.c_str(), PROGRESS_BAR_WIDTH);
		}
	}
#endif
}
} // namespace duckdb
