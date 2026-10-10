if (!exists("data_file")) data_file = "throughput.csv"
if (!exists("output_file")) output_file = "throughput.png"
if (!exists("profile_label")) profile_label = "UML profile"

set terminal pngcairo size 1200, 720 enhanced font "Sans,12"
set output output_file
set datafile separator comma
set title sprintf("nginx throughput: %s", profile_label)
set xlabel "wrk connections"
set ylabel "Requests per second"
set style data histograms
set style histogram clustered gap 1
set style fill solid 0.8 border -1
set boxwidth 0.9
set key outside top center horizontal
set grid ytics
set yrange [0:*]
plot data_file every ::1 using 2:xtic(1) title "worker processes 1", \
     '' every ::1 using 3 title "worker processes 4"
