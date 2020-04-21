# Pollard's kangaroo for SECPK1

A simple Pollard's kangaroo solver for SECPK1

Structure of the input file:
* All values are in hex format
* Public keys can be given either in compressed or uncompressed format

```
Start range
End range
Key #1
Key #2
...
```

ex

```
49dccfd96dc5df56487436f5a1b18c4f5d34f65ddb48cb5e0000000000000000
49dccfd96dc5df56487436f5a1b18c4f5d34f65ddb48cb5effffffffffffffff
0459A3BFDAD718C9D3FAC7C187F1139F0815AC5D923910D516E186AFDA28B221DC994327554CED887AAE5D211A2407CDD025CFC3779ECB9C9D7F2F1A1DDF3E9FF8
0335BB25364370D4DD14A9FC2B406D398C4B53C85BE58FCC7297BD34004602EBEC
```

# How it works

The program uses 2 herds of kangaroos, a tame herd and a wild herd. When 2 kangoroo (a wild one and a tame one) collide, the key
can be solved. Due to the birtday paradox, a collision happens (in average) after 2*sqrt(k2-k1) iterations, the 2 herds have the 
same size. Here is a brief description of the algoritm:

We have to solve P = k.G, we know that k lies in the range ]k1,k2], G is the SecpK1 generator point.

n = floor(sqrt(k2-k1))+1

* Create a jump table point jP = [G,2G,4G,8G,...2^nG], 
* Create a jump distance table jD = [1,2,4,8,....2^n]
 
tame<sub>i</sub> = rand(0..k2-k1)</br>
tamePos<sub>i</sub> = tame<sub>i</sub>.G</br>
wild<sub>i</sub> = rand(0..k2-k1)</br>
wildPos<sub>i</sub> = P + wild<sub>i</sub>.G</br>
 
while not collision between tamePos<sub>i</sub> and wildPos<sub>i</sub> {</br>
&nbsp;&nbsp; tamePos<sub>i</sub> = tamePos<sub>i</sub> + jP[tamePos<sub>i</sub>.x % n]</br>
&nbsp;&nbsp;  tame<sub>i</sub> += jD[tamePos<sub>i</sub>.x % n]</br>
&nbsp;&nbsp;  wildPos<sub>i</sub> = wildPos<sub>i</sub> + jP[wildPos<sub>i</sub>.x % n]</br>
&nbsp;&nbsp;  wild<sub>i</sub> += jD[wildPos<sub>i</sub>.x % n]</br>
}</br>

(t,w) = index of collision</br>
K = k1 + tame<sub>t</sub> - wild<sub>w</sub></br>



