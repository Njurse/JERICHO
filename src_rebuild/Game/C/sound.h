#ifndef SOUND_H
#define SOUND_H

enum ChanFlags
{
	CHAN_LOOP			= (1 << 0),
	CHAN_LOCKED			= (1 << 1)
};

struct CHANNEL_DATA
{
	SpuVoiceAttr attr;
	VECTOR* srcposition;
	VECTOR position;
	LONGVECTOR3* srcvelocity;

	int samplerate;
	int srcvolume;
	int volumeScale;
	int cameradist;
	int lastcameradist;

	u_short srcpitch;
	u_short dopplerScale;

	u_short time;
	u_char flags;
	char player;
};

extern CHANNEL_DATA channels[16];

extern int gMasterVolume;
extern int gMusicVolume;
extern int gSoundMode;

extern int Song_ID;
extern int Song_SetPos;
extern int VABID;
extern int music_paused;

extern void InitSound(); // 0x000790E4
extern void ResetSound(); // 0x00079250

extern void AllocateReverb(int mode, int depth); // 0x0007AB3C
extern void SetReverbState(int on); // 0x0007A6C0
extern void SetReverbInGameState(int on); // 0x0007A704
extern int SetReverbChannelState(int ch, int on); // 0x0007A75C

extern void SetMasterVolume(int vol); // 0x0007A7B0
extern void SetXMVolume(int volume); // 0x0007B074

extern char SetPlayerOwnsChannel(int chan, char player); // 0x0007ABA8

extern int StartSound(int channel, int bank, int sample, int volume, int pitch); // 0x0007A7F8
extern int Start3DTrackingSound(int channel, int bank, int sample, VECTOR *position, LONGVECTOR3* velocity); // 0x0007A994
extern int Start3DSoundVolPitch(int channel, int bank, int sample, int x, int y, int z, int volume, int pitch); // 0x0007A894

extern void SetChannelPitch(int channel, int pitch); // 0x00079504
extern void SetChannelVolume(int channel, int volume, int proximity); // 0x0007AA78
extern void SetChannelPosition3(int channel, VECTOR *position, LONGVECTOR3* velocity, int volume, int pitch, int proximity); // 0x0007ABCC

extern void PauseSFX(); // 0x00079794
extern void PauseSound(); // 0x0007ACD8

extern void UnPauseXM(); // 0x00079958
extern void UnPauseSFX(); // 0x000799B0

extern void UnPauseSound(); // 0x0007AD10

extern int GetFreeChannel(int force = 1); // 0x00079E04
extern void StopChannel(int channel); // 0x0007AD40
extern void StopAllChannels(); // 0x0007ADF4

extern void LockChannel(int channel); // 0x0007AE34
extern void UnlockChannel(int c); // 0x0007AE64

extern int LoadSoundBank(char *address, int length, int bank); // 0x0007AE90
extern int LoadSoundBankDynamic(char *address, int length, int dbank); // 0x00079BCC

/* How many samples a dynamically loaded bank currently holds. gamesnd uses
 * this to give an appended car bank a real sample block instead of deriving
 * one from the resident slot index. */
extern int GetSoundBankSampleCount(int bank);

/* Could `samples` more samples be appended to dynamic bank `bank`? The
 * capacity is fixed; gamesnd asks before appending an imported car bank so it
 * can reclaim the appended region first instead of failing the load. */
extern int SoundBankHasRoom(int bank, int samples);

/* Fixed sample capacity of every dynamic bank; used by gamesnd's level-load
 * check that one 3-sample bank per resident slot still fits. */
extern int GetSoundBankCapacity(void);

/* Append zero-length metadata entries until the bank's sample count is a
 * multiple of `multiple`. A car bank's sample block is addressed as block*3
 * (engine, idle, horn), so an imported bank must start on a 3-sample boundary
 * even when the level's special banks did not end on one. */
extern void AlignSoundBankSamples(int bank, int multiple);

extern void UpdateXMSamples(int num_samps); // 0x0007B1C4

extern void FreeXM(); // 0x0007AF84
extern void StartXM(int reverb); // 0x0007AFD0
extern void StopXM(); // 0x0007B050

extern int FESound(int sample); // 0x0007A534

extern int jsqrt(u_int a); // 0x0005270C

#endif
