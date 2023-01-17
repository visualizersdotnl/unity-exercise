#include <stdio.h>
#include <math.h>


unsigned GetWordScore(size_t length) /* const */
{
	if (length > 8)
		length = 8;

	constexpr unsigned kLUT[] = { 1, 1, 2, 3, 5, 11 };
	return kLUT[length-3];
}

unsigned GetWordScore2(size_t length) /* const */
{
	if (length>8) length=8;
	length-=3;
	
	int ret=0;
	
	ret += length>>1;
	ret += (length+1)>>2;
	ret += length>>2;
	ret += (length+3>>3)<<2;
	ret += (length+3>>3<<1);
	

	return ret+1;
}


int main () {

	for (int i=3; i<20; i++) {
	
		printf("%d: %d - %d\n", i, GetWordScore(i), GetWordScore2(i));
	}

	return 0;
}
