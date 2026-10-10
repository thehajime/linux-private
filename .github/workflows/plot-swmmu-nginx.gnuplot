if (!exists("data_file")) data_file = "throughput.csv"
if (!exists("output_file")) output_file = "throughput.png"
if (!exists("profile_label")) profile_label = "UML profile"
if (!exists("plot_workers_1")) plot_workers_1 = 1
if (!exists("plot_workers_4")) plot_workers_4 = 1

set terminal pngcairo size 1200, 720 enhanced font "Sans,12"
set output output_file
set datafile separator comma
set datafile missing "NaN"
set title sprintf("nginx throughput: %s", profile_label)
set xlabel "wrk connections"
set ylabel "Requests per second"
set style data linespoints
set pointsize 1.2
set key outside top center horizontal
set grid ytics
set yrange [0:*]

plot_cmd = ""
if (plot_workers_1) {
    plot_cmd = sprintf("'%s' every ::1 using 0:2:xtic(1) with linespoints title 'worker processes 1'", data_file)
}
if (plot_workers_4) {
    if (strlen(plot_cmd) > 0) plot_cmd = plot_cmd . ", "
    if (strlen(plot_cmd) > 0) {
        plot_cmd = plot_cmd . "'' every ::1 using 0:3 with linespoints title 'worker processes 4'"
    } else {
        plot_cmd = sprintf("'%s' every ::1 using 0:3:xtic(1) with linespoints title 'worker processes 4'", data_file)
    }
}
eval(sprintf("plot %s", plot_cmd))
