//! Helpers for [uidgid file](https://pagure.io/setup/blob/master/f/uidgid).
// SPDX-License-Identifier: Apache-2.0 OR MIT

/// Static UIDs and GIDs, sourced from <https://pagure.io/setup/blob/master/f/uidgid>.
static SETUP_UIDGID_CONTENT: &'static str = include_str!("uidgid");

pub struct StaticEntries {
    groups: Vec<(String, u32, bool)>,
    users: Vec<(String, u64, u32, String, String, String, bool)>,
}

impl StaticEntries {
    pub fn new() -> Self {
        let (groups, users) = parse_uidgid_content(SETUP_UIDGID_CONTENT);
        Self { groups, users }
    }

    pub fn sysusers_entries(&self) -> Vec<String> {
        let mut lines = vec![];
        for group in &self.groups {
            let group_from_setup_rpm = group.2;
            if group_from_setup_rpm {
                let entry = format!("g {} {}", group.0, group.1);
                lines.push(entry);
            }
        }
        for user in &self.users {
            let group_from_setup_rpm = user.6;
            if group_from_setup_rpm {
                let entry = format!(
                    "u {} {}:{} {} {} {}",
                    user.0, user.1, user.2, user.3, user.4, user.5
                );
                lines.push(entry);
            }
        }
        lines
    }
}

fn parse_uidgid_content(
    input: &str,
) -> (
    Vec<(String, u32, bool)>,
    Vec<(String, u64, u32, String, String, String, bool)>,
) {
    let mut users = vec![];
    let mut groups = vec![];
    for line in input.lines() {
        // Skip empty and commented lines.
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        };

        let mut fields: Vec<_> = line.split('\t').collect();
        if fields.len() < 7 {
            unreachable!("Malformed uidgid line '{}'", line);
        }

        let gecos = fields.pop().expect("Missing GECOS field");
        let packages = fields.pop().expect("Missing PACKAGES field");
        let shell = fields.pop().expect("Missing SHELL field");
        let _empty = fields.pop().expect("Missing _EMPTY field");
        let home = fields.pop().expect("Missing HOME field");
        let gid_label = fields.pop().expect("Missing GID field");
        let uid_label = fields.pop().expect("Missing UID field");
        let name = fields.pop().expect("Missing NAME field");
        drop(fields);

        let entry_from_setup_rpm = packages == "setup";

        // Group creation.
        if let Ok(id) = gid_label.parse::<u32>() {
            groups.push((name.to_string(), id, entry_from_setup_rpm));
        };

        // User creation.
        let uid = match uid_label.parse::<u64>() {
            Ok(id) => id,
            _ => continue,
        };
        let primary_group = gid_label.trim_matches(|c| c == '(' || c == ')');
        let primary_gid = match primary_group.parse::<u32>() {
            Ok(id) => id,
            _ => unreachable!("Malformed primary_gid '{}'", line),
        };
        users.push((
            name.to_string(),
            uid,
            primary_gid,
            gecos.to_string(),
            home.to_string(),
            shell.to_string(),
            entry_from_setup_rpm,
        ));
    }
    (groups, users)
}

#[cfg(test)]
mod test {
    use super::*;

    #[test]
    fn test_parser() {}

    #[test]
    fn test_content_parsing() {
        let static_ids = StaticEntries::new();
        assert!(!static_ids.groups.is_empty());

        // Entries taken from https://pagure.io/setup/blob/master/f/group
        let known_gids_from_setup = vec![
            ("root".to_string(), 0u32),
            ("bin".to_string(), 1),
            ("daemon".to_string(), 2),
            ("sys".to_string(), 3),
            ("adm".to_string(), 4),
            ("tty".to_string(), 5),
            ("disk".to_string(), 6),
            ("lp".to_string(), 7),
            ("mem".to_string(), 8),
            ("kmem".to_string(), 9),
            ("wheel".to_string(), 10),
            ("cdrom".to_string(), 11),
            ("mail".to_string(), 12),
            ("ftp".to_string(), 50),
            ("man".to_string(), 15),
            ("dialout".to_string(), 18),
            ("floppy".to_string(), 19),
            ("games".to_string(), 20),
            ("tape".to_string(), 33),
            ("video".to_string(), 39),
            ("lock".to_string(), 54),
            ("audio".to_string(), 63),
            ("users".to_string(), 100),
            ("nobody".to_string(), 65534),
        ];
        let basic_gids: Vec<_> = static_ids
            .groups
            .iter()
            .cloned()
            .filter_map(
                |(name, id, is_basic)| {
                    if is_basic {
                        Some((name, id))
                    } else {
                        None
                    }
                },
            )
            .collect();
        assert_eq!(known_gids_from_setup, basic_gids);

        // Entries taken from https://pagure.io/setup/blob/master/f/passwd
        let known_uids_from_setup = vec![
            ("root".to_string(), 0u64, 0u32),
            ("bin".to_string(), 1, 1),
            ("daemon".to_string(), 2, 2),
            ("adm".to_string(), 3, 4),
            ("lp".to_string(), 4, 7),
            ("sync".to_string(), 5, 0),
            ("shutdown".to_string(), 6, 0),
            ("halt".to_string(), 7, 0),
            ("mail".to_string(), 8, 12),
            ("operator".to_string(), 11, 0),
            ("games".to_string(), 12, 100),
            ("ftp".to_string(), 14, 50),
            ("nobody".to_string(), 65534, 65534),
        ];
        let basic_uids: Vec<_> = static_ids
            .users
            .iter()
            .cloned()
            .filter_map(|(name, uid, gid, _gecos, _home, _shell, is_basic)| {
                if is_basic {
                    Some((name, uid, gid))
                } else {
                    None
                }
            })
            .collect();
        assert_eq!(known_uids_from_setup, basic_uids);

        let computed_lines = static_ids.sysusers_entries();
        let expected_lines = vec![
            "g root 0",
            "g bin 1",
            "g daemon 2",
            "g sys 3",
            "g adm 4",
            "g tty 5",
            "g disk 6",
            "g lp 7",
            "g mem 8",
            "g kmem 9",
            "g wheel 10",
            "g cdrom 11",
            "g mail 12",
            "g ftp 50",
            "g man 15",
            "g dialout 18",
            "g floppy 19",
            "g games 20",
            "g tape 33",
            "g video 39",
            "g lock 54",
            "g audio 63",
            "g users 100",
            "g nobody 65534",
        ];
        assert_eq!(computed_lines, expected_lines);
    }
}
