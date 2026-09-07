use Name;

#[derive(Debug, Clone, Copy)]
pub struct Record<'a>(pub Name<'a>);

impl<'a> ToString for Record<'a> {
    #[inline]
    fn to_string(&self) -> String {
        self.0.to_string()
    }
}

impl<'a> super::Record<'a> for Record<'a> {

    const TYPE: isize = 12;

    fn parse(rdata: &'a [u8], original: &'a [u8]) -> super::RDataResult<'a> {
        let name = Name::scan(rdata, original)?;
        let record = Record(name);
        Ok(super::RData::PTR(record))
    }
}
