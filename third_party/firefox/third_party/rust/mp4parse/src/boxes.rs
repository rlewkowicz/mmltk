// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
use std::fmt;

#[allow(dead_code)]
struct Vec;
#[allow(dead_code)]
struct Box;
#[allow(dead_code)]
struct HashMap;
#[allow(dead_code)]
struct String;

macro_rules! box_database {
    ($($(#[$attr:meta])* $boxenum:ident $boxtype:expr),*,) => {
        #[derive(Clone, Copy, PartialEq, Eq)]
        pub enum BoxType {
            $($(#[$attr])* $boxenum),*,
            UnknownBox(u32),
        }

        impl From<u32> for BoxType {
            fn from(t: u32) -> BoxType {
                use self::BoxType::*;
                match t {
                    $($(#[$attr])* $boxtype => $boxenum),*,
                    _ => UnknownBox(t),
                }
            }
        }

        impl From<BoxType> for u32 {
            fn from(b: BoxType) -> u32 {
                use self::BoxType::*;
                match b {
                    $($(#[$attr])* $boxenum => $boxtype),*,
                    UnknownBox(t) => t,
                }
            }
        }

    }
}

impl fmt::Debug for BoxType {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        let fourcc: FourCC = From::from(*self);
        fourcc.fmt(f)
    }
}

#[derive(Default, Eq, Hash, PartialEq, Clone)]
pub struct FourCC {
    pub value: [u8; 4],
}

impl From<u32> for FourCC {
    fn from(number: u32) -> FourCC {
        FourCC {
            value: number.to_be_bytes(),
        }
    }
}

impl From<BoxType> for FourCC {
    fn from(t: BoxType) -> FourCC {
        let box_num: u32 = Into::into(t);
        From::from(box_num)
    }
}

impl From<[u8; 4]> for FourCC {
    fn from(v: [u8; 4]) -> FourCC {
        FourCC { value: v }
    }
}

impl fmt::Debug for FourCC {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match std::str::from_utf8(&self.value) {
            Ok(s) => f.write_str(s),
            Err(_) => self.value.fmt(f),
        }
    }
}

impl fmt::Display for FourCC {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        f.write_str(std::str::from_utf8(&self.value).unwrap_or("null"))
    }
}

impl PartialEq<&[u8; 4]> for FourCC {
    fn eq(&self, other: &&[u8; 4]) -> bool {
        self.value.eq(*other)
    }
}

box_database!(
    FileTypeBox                       0x6674_7970, 
    MediaDataBox                      0x6d64_6174, 
    PrimaryItemBox                    0x7069_746d, 
    ItemDataBox                       0x6964_6174, 
    ItemInfoBox                       0x6969_6e66, 
    ItemInfoEntry                     0x696e_6665, 
    ItemLocationBox                   0x696c_6f63, 
    MovieBox                          0x6d6f_6f76, 
    MovieHeaderBox                    0x6d76_6864, 
    TrackBox                          0x7472_616b, 
    TrackHeaderBox                    0x746b_6864, 
    TrackReferenceBox                 0x7472_6566, 
    AuxiliaryBox                      0x6175_786C, 
    EditBox                           0x6564_7473, 
    MediaBox                          0x6d64_6961, 
    EditListBox                       0x656c_7374, 
    MediaHeaderBox                    0x6d64_6864, 
    HandlerBox                        0x6864_6c72, 
    MediaInformationBox               0x6d69_6e66, 
    ItemReferenceBox                  0x6972_6566, 
    ItemPropertiesBox                 0x6970_7270, 
    ItemPropertyContainerBox          0x6970_636f, 
    ItemPropertyAssociationBox        0x6970_6d61, 
    ColourInformationBox              0x636f_6c72, 
    MasteringDisplayColourVolumeBox   0x6d64_6376, 
    ContentLightLevelBox              0x636c_6c69, 
    ImageSpatialExtentsProperty       0x6973_7065, 
    PixelAspectRatioBox               0x7061_7370, 
    PixelInformationBox               0x7069_7869, 
    AuxiliaryTypeProperty             0x6175_7843, 
    CleanApertureBox                  0x636c_6170, 
    ImageRotation                     0x6972_6f74, 
    ImageMirror                       0x696d_6972, 
    OperatingPointSelectorProperty    0x6131_6f70, 
    AV1LayeredImageIndexingProperty   0x6131_6c78, 
    LayerSelectorProperty             0x6c73_656c, 
    SampleTableBox                    0x7374_626c, 
    SampleDescriptionBox              0x7374_7364, 
    TimeToSampleBox                   0x7374_7473, 
    SampleToChunkBox                  0x7374_7363, 
    SampleSizeBox                     0x7374_737a, 
    ChunkOffsetBox                    0x7374_636f, 
    ChunkLargeOffsetBox               0x636f_3634, 
    SyncSampleBox                     0x7374_7373, 
    AVCSampleEntry                    0x6176_6331, 
    AVC3SampleEntry                   0x6176_6333, 
    AVCConfigurationBox               0x6176_6343, 
    H263SampleEntry                   0x7332_3633, 
    H263SpecificBox                   0x6432_3633, 
    HEV1SampleEntry                   0x6865_7631, 
    HVC1SampleEntry                   0x6876_6331, 
    HEVCConfigurationBox              0x6876_6343, 
    MP4AudioSampleEntry               0x6d70_3461, 
    MP4VideoSampleEntry               0x6d70_3476, 
    #[cfg(feature = "3gpp")]
    AMRNBSampleEntry                  0x7361_6d72, 
    #[cfg(feature = "3gpp")]
    AMRWBSampleEntry                  0x7361_7762, 
    #[cfg(feature = "3gpp")]
    AMRSpecificBox                    0x6461_6d72, 
    ESDBox                            0x6573_6473, 
    VP8SampleEntry                    0x7670_3038, 
    VP9SampleEntry                    0x7670_3039, 
    VPCodecConfigurationBox           0x7670_6343, 
    AV1SampleEntry                    0x6176_3031, 
    AV1CodecConfigurationBox          0x6176_3143, 
    FLACSampleEntry                   0x664c_6143, 
    FLACSpecificBox                   0x6466_4c61, 
    OpusSampleEntry                   0x4f70_7573, 
    OpusSpecificBox                   0x644f_7073, 
    ProtectedVisualSampleEntry        0x656e_6376, 
    ProtectedAudioSampleEntry         0x656e_6361, 
    MovieExtendsBox                   0x6d76_6578, 
    MovieExtendsHeaderBox             0x6d65_6864, 
    QTWaveAtom                        0x7761_7665, 
    ProtectionSystemSpecificHeaderBox 0x7073_7368, 
    SchemeInformationBox              0x7363_6869, 
    TrackEncryptionBox                0x7465_6e63, 
    ProtectionSchemeInfoBox           0x7369_6e66, 
    OriginalFormatBox                 0x6672_6d61, 
    SchemeTypeBox                     0x7363_686d, 
    MP3AudioSampleEntry               0x2e6d_7033, 
    CompositionOffsetBox              0x6374_7473, 
    LPCMAudioSampleEntry              0x6c70_636d, 
    ALACSpecificBox                   0x616c_6163, 
    UuidBox                           0x7575_6964, 
    MetadataBox                       0x6d65_7461, 
    MetadataHeaderBox                 0x6d68_6472, 
    MetadataItemKeysBox               0x6b65_7973, 
    MetadataItemListEntry             0x696c_7374, 
    MetadataItemDataEntry             0x6461_7461, 
    MetadataItemNameBox               0x6e61_6d65, 
    #[cfg(feature = "meta-xml")]
    MetadataXMLBox                    0x786d_6c20, 
    #[cfg(feature = "meta-xml")]
    MetadataBXMLBox                   0x6278_6d6c, 
    UserdataBox                       0x7564_7461, 
    AlbumEntry                        0xa961_6c62, 
    ArtistEntry                       0xa941_5254, 
    ArtistLowercaseEntry              0xa961_7274, 
    AlbumArtistEntry                  0x6141_5254, 
    CommentEntry                      0xa963_6d74, 
    DateEntry                         0xa964_6179, 
    TitleEntry                        0xa96e_616d, 
    CustomGenreEntry                  0xa967_656e, 
    StandardGenreEntry                0x676e_7265, 
    TrackNumberEntry                  0x7472_6b6e, 
    DiskNumberEntry                   0x6469_736b, 
    ComposerEntry                     0xa977_7274, 
    EncoderEntry                      0xa974_6f6f, 
    EncodedByEntry                    0xa965_6e63, 
    TempoEntry                        0x746d_706f, 
    CopyrightEntry                    0x6370_7274, 
    CompilationEntry                  0x6370_696c, 
    CoverArtEntry                     0x636f_7672, 
    AdvisoryEntry                     0x7274_6e67, 
    RatingEntry                       0x7261_7465, 
    GroupingEntry                     0xa967_7270, 
    MediaTypeEntry                    0x7374_696b, 
    PodcastEntry                      0x7063_7374, 
    CategoryEntry                     0x6361_7467, 
    KeywordEntry                      0x6b65_7977, 
    PodcastUrlEntry                   0x7075_726c, 
    PodcastGuidEntry                  0x6567_6964, 
    DescriptionEntry                  0x6465_7363, 
    LongDescriptionEntry              0x6c64_6573, 
    LyricsEntry                       0xa96c_7972, 
    TVNetworkNameEntry                0x7476_6e6e, 
    TVShowNameEntry                   0x7476_7368, 
    TVEpisodeNameEntry                0x7476_656e, 
    TVSeasonNumberEntry               0x7476_736e, 
    TVEpisodeNumberEntry              0x7476_6573, 
    PurchaseDateEntry                 0x7075_7264, 
    GaplessPlaybackEntry              0x7067_6170, 
    OwnerEntry                        0x6f77_6e72, 
    HDVideoEntry                      0x6864_7664, 
    SortNameEntry                     0x736f_6e6d, 
    SortAlbumEntry                    0x736f_616c, 
    SortArtistEntry                   0x736f_6172, 
    SortAlbumArtistEntry              0x736f_6161, 
    SortComposerEntry                 0x736f_636f, 
);
