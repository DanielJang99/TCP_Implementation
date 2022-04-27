import matplotlib.pyplot as plt
from argparse import ArgumentParser
parser = ArgumentParser(description="plot")
args = parser.parse_args()


f1 = open("CWND.csv")
cwnd = []
time = []


for n in f1:
    n = n.strip().split(' : ')
    time.append(int(n[0]))
    cwnd.append(int(n[1]))


fig = plt.figure(figsize=(21,3), facecolor='w')

plt.plot(time, cwnd)

plt.xlabel("Time Elasped (ms)", fontsize=20)
plt.ylabel("CWND", fontsize=20)

# plt.savefig(args.dir+'/throughput.pdf',dpi=1000,bbox_inches='tight')
plt.savefig("cwnd_plot.png")
plt.show()
f1.close()
